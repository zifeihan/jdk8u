/*
 * Copyright (c) 1997, 2018, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "classfile/systemDictionary.hpp"
#include "code/codeCache.hpp"
#include "code/compiledIC.hpp"
#include "code/icBuffer.hpp"
#include "code/nmethod.hpp"
#include "code/vtableStubs.hpp"
#include "interpreter/interpreter.hpp"
#include "interpreter/linkResolver.hpp"
#include "memory/metadataFactory.hpp"
#include "memory/oopFactory.hpp"
#include "oops/method.hpp"
#include "oops/oop.inline.hpp"
#include "oops/symbol.hpp"
#include "runtime/icache.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubRoutines.hpp"
#include "utilities/events.hpp"


// Every time a compiled IC is changed or its type is being accessed,
// either the CompiledIC_lock must be set or we must be at a safe point.

//-----------------------------------------------------------------------------
// Low-level access to an inline cache. Private, since they might not be
// MT-safe to use.

void* CompiledIC::cached_value() const {
  assert (CompiledIC_lock->is_locked() || SafepointSynchronize::is_at_safepoint(), "");
  assert (!is_optimized(), "an optimized virtual call does not have a cached metadata");

  if (!is_in_transition_state()) {
    void* data = (void*)_value->data();
    // If we let the metadata value here be initialized to zero...
    assert(data != NULL || Universe::non_oop_word() == NULL,
           "no raw nulls in CompiledIC metadatas, because of patching races");
    return (data == (void*)Universe::non_oop_word()) ? NULL : data;
  } else {
    return InlineCacheBuffer::cached_value_for((CompiledIC *)this);
  }
}


void CompiledIC::internal_set_ic_destination(address entry_point, bool is_icstub, void* cache, bool is_icholder) {
  assert(entry_point != NULL, "must set legal entry point");
  assert(CompiledIC_lock->is_locked() || SafepointSynchronize::is_at_safepoint(), "");
  assert (!is_optimized() || cache == NULL, "an optimized virtual call does not have a cached metadata");
  assert (cache == NULL || cache != (Metadata*)badOopVal, "invalid metadata");

  assert(!is_icholder || is_icholder_entry(entry_point), "must be");

  // Don't use ic_destination for this test since that forwards
  // through ICBuffer instead of returning the actual current state of
  // the CompiledIC.
  if (is_icholder_entry(_ic_call->destination())) {
    // When patching for the ICStub case the cached value isn't
    // overwritten until the ICStub copied into the CompiledIC during
    // the next safepoint.  Make sure that the CompiledICHolder* is
    // marked for release at this point since it won't be identifiable
    // once the entry point is overwritten.
    InlineCacheBuffer::queue_for_release((CompiledICHolder*)_value->data());
  }

  if (TraceCompiledIC) {
    tty->print("  ");
    print_compiled_ic();
    tty->print(" changing destination to " INTPTR_FORMAT, p2i(entry_point));
    if (!is_optimized()) {
      tty->print(" changing cached %s to " INTPTR_FORMAT, is_icholder ? "icholder" : "metadata", p2i((address)cache));
    }
    if (is_icstub) {
      tty->print(" (icstub)");
    }
    tty->cr();
  }

  {
    MutexLockerEx pl(SafepointSynchronize::is_at_safepoint() ? NULL : Patching_lock, Mutex::_no_safepoint_check_flag);
#ifdef ASSERT
    CodeBlob* cb = CodeCache::find_blob_unsafe(_ic_call);
    assert(cb != NULL && cb->is_nmethod(), "must be nmethod");
#endif
     _ic_call->set_destination_mt_safe(entry_point);
  }

  if (is_optimized() || is_icstub) {
    // Optimized call sites don't have a cache value and ICStub call
    // sites only change the entry point.  Changing the value in that
    // case could lead to MT safety issues.
    assert(cache == NULL, "must be null");
    return;
  }

  if (cache == NULL)  cache = (void*)Universe::non_oop_word();

  _value->set_data((intptr_t)cache);
}


void CompiledIC::set_ic_destination(ICStub* stub) {
  internal_set_ic_destination(stub->code_begin(), true, NULL, false);
}



address CompiledIC::ic_destination() const {
 assert (CompiledIC_lock->is_locked() || SafepointSynchronize::is_at_safepoint(), "");
 if (!is_in_transition_state()) {
   return _ic_call->destination();
 } else {
   return InlineCacheBuffer::ic_destination_for((CompiledIC *)this);
 }
}


bool CompiledIC::is_in_transition_state() const {
  assert (CompiledIC_lock->is_locked() || SafepointSynchronize::is_at_safepoint(), "");
  return InlineCacheBuffer::contains(_ic_call->destination());
}


bool CompiledIC::is_icholder_call() const {
  assert (CompiledIC_lock->is_locked() || SafepointSynchronize::is_at_safepoint(), "");
  return !_is_optimized && is_icholder_entry(ic_destination());
}

// Returns native address of 'call' instruction in inline-cache. Used by
// the InlineCacheBuffer when it needs to find the stub.
address CompiledIC::stub_address() const {
  assert(is_in_transition_state(), "should only be called when we are in a transition state");
  return _ic_call->destination();
}

// Clears the IC stub if the compiled IC is in transition state
void CompiledIC::clear_ic_stub() {
  if (is_in_transition_state()) {
    ICStub* stub = ICStub_from_destination_address(stub_address());
    stub->clear();
  }
}


//-----------------------------------------------------------------------------
// High-level access to an inline cache. Guaranteed to be MT-safe.

void CompiledIC::initialize_from_iter(RelocIterator* iter) {
  assert(iter->addr() == _ic_call->instruction_address(), "must find ic_call");

  if (iter->type() == relocInfo::virtual_call_type) {
    virtual_call_Relocation* r = iter->virtual_call_reloc();
    _is_optimized = false;
    _value = nativeMovConstReg_at(r->cached_value());
  } else {
    assert(iter->type() == relocInfo::opt_virtual_call_type, "must be a virtual call");
    _is_optimized = true;
    _value = NULL;
  }
}

CompiledIC::CompiledIC(nmethod* nm, NativeCall* call)
  : _ic_call(call)
{
  address ic_call = _ic_call->instruction_address();

  assert(ic_call != NULL, "ic_call address must be set");
  assert(nm != NULL, "must pass nmethod");
  assert(nm->contains(ic_call), "must be in nmethod");

  // Search for the ic_call at the given address.
  RelocIterator iter(nm, ic_call, ic_call+1);
  bool ret = iter.next();
  assert(ret == true, "relocInfo must exist at this address");
  assert(iter.addr() == ic_call, "must find ic_call");

  initialize_from_iter(&iter);
}

CompiledIC::CompiledIC(RelocIterator* iter)
  : _ic_call(nativeCall_at(iter->addr()))
{
  address ic_call = _ic_call->instruction_address();

  nmethod* nm = iter->code();
  assert(ic_call != NULL, "ic_call address must be set");
  assert(nm != NULL, "must pass nmethod");
  assert(nm->contains(ic_call), "must be in nmethod");

  initialize_from_iter(iter);
}

bool CompiledIC::set_to_megamorphic(CallInfo* call_info, Bytecodes::Code bytecode, TRAPS) {
  assert(CompiledIC_lock->is_locked() || SafepointSynchronize::is_at_safepoint(), "");
  assert(!is_optimized(), "cannot set an optimized virtual call to megamorphic");
  assert(is_call_to_compiled() || is_call_to_interpreted(), "going directly to megamorphic?");

  address entry;
  if (call_info->call_kind() == CallInfo::itable_call) {
    assert(bytecode == Bytecodes::_invokeinterface, "");
    int itable_index = call_info->itable_index();
    entry = VtableStubs::find_itable_stub(itable_index);
    if (entry == false) {
      return false;
    }
#ifdef ASSERT
    int index = call_info->resolved_method()->itable_index();
    assert(index == itable_index, "CallInfo pre-computes this");
    InstanceKlass* k = call_info->resolved_method()->method_holder();
    assert(k->verify_itable_index(itable_index), "sanity check");
#endif //ASSERT
    CompiledICHolder* holder = new CompiledICHolder(call_info->resolved_method()->method_holder(),
                                                    call_info->resolved_klass()(), false);
    holder->claim();
    InlineCacheBuffer::create_transition_stub(this, holder, entry);
  } else {
    assert(call_info->call_kind() == CallInfo::vtable_call, "either itable or vtable");
    // Can be different than selected_method->vtable_index(), due to package-private etc.
    int vtable_index = call_info->vtable_index();
    assert(call_info->resolved_klass()->verify_vtable_index(vtable_index), "sanity check");
    entry = VtableStubs::find_vtable_stub(vtable_index);
    if (entry == NULL) {
      return false;
    }
    InlineCacheBuffer::create_transition_stub(this, NULL, entry);
  }

  if (TraceICs) {
    ResourceMark rm;
    tty->print_cr ("IC@" INTPTR_FORMAT ": to megamorphic %s entry: " INTPTR_FORMAT,
                   p2i(instruction_address()), call_info->selected_method()->print_value_string(), p2i(entry));
  }

  // We can't check this anymore. With lazy deopt we could have already
  // cleaned this IC entry before we even return. This is possible if
  // we ran out of space in the inline cache buffer trying to do the
  // set_next and we safepointed to free up space. This is a benign
  // race because the IC entry was complete when we safepointed so
  // cleaning it immediately is harmless.
  // assert(is_megamorphic(), "sanity check");
  return true;
}


// true if destination is megamorphic stub
bool CompiledIC::is_megamorphic() const {
  assert(CompiledIC_lock->is_locked() || SafepointSynchronize::is_at_safepoint(), "");
  assert(!is_optimized(), "an optimized call cannot be megamorphic");

  // Cannot rely on cached_value. It is either an interface or a method.
  return VtableStubs::entry_point(ic_destination()) != NULL;
}

bool CompiledIC::is_call_to_compiled() const {
  assert (CompiledIC_lock->is_locked() || SafepointSynchronize::is_at_safepoint(), "");

  // Use unsafe, since an inline cache might point to a zombie method. However, the zombie
  // method is guaranteed to still exist, since we only remove methods after all inline caches
  // has been cleaned up
  CodeBlob* cb = CodeCache::find_blob_unsafe(ic_destination());
  bool is_monomorphic = (cb != NULL && cb->is_nmethod());
  // Check that the cached_value is a klass for non-optimized monomorphic calls
  // This assertion is invalid for compiler1: a call that does not look optimized (no static stub) can be used
  // for calling directly to vep without using the inline cache (i.e., cached_value == NULL)
#ifdef ASSERT
  CodeBlob* caller = CodeCache::find_blob_unsafe(instruction_address());
  bool is_c1_method = caller->is_compiled_by_c1();
  assert( is_c1_method ||
         !is_monomorphic ||
         is_optimized() ||
         !caller->is_alive() ||
         (cached_metadata() != NULL && cached_metadata()->is_klass()), "sanity check");
#endif // ASSERT
  return is_monomorphic;
}


bool CompiledIC::is_call_to_interpreted() const {
  assert (CompiledIC_lock->is_locked() || SafepointSynchronize::is_at_safepoint(), "");
  // Call to interpreter if destination is either calling to a stub (if it
  // is optimized), or calling to an I2C blob
  bool is_call_to_interpreted = false;
  if (!is_optimized()) {
    // must use unsafe because the destination can be a zombie (and we're cleaning)
    // and the print_compiled_ic code wants to know if site (in the non-zombie)
    // is to the interpreter.
    CodeBlob* cb = CodeCache::find_blob_unsafe(ic_destination());
    is_call_to_interpreted = (cb != NULL && cb->is_adapter_blob());
    assert(!is_call_to_interpreted || (is_icholder_call() && cached_icholder() != NULL), "sanity check");
  } else {
    // Check if we are calling into our own codeblob (i.e., to a stub)
    CodeBlob* cb = CodeCache::find_blob(_ic_call->instruction_address());
    address dest = ic_destination();
#ifdef ASSERT
    {
      CodeBlob* db = CodeCache::find_blob_unsafe(dest);
      assert(!db->is_adapter_blob(), "must use stub!");
    }
#endif /* ASSERT */
    is_call_to_interpreted = cb->contains(dest);
  }
  return is_call_to_interpreted;
}
/*bool CompiledIC::is_icholder_call_site(virtual_call_Relocation* call_site) {
  // This call site might have become stale so inspect it carefully.
  NativeCall* call = nativeCall_at(call_site->addr());
  return is_icholder_entry(call->destination());
}*/

void CompiledIC::set_to_clean(bool in_use) {
  assert(SafepointSynchronize::is_at_safepoint() || CompiledIC_lock->is_locked() , "MT-unsafe call");
  if (TraceInlineCacheClearing || TraceICs) {
    tty->print_cr("IC@" INTPTR_FORMAT ": set to clean", p2i(instruction_address()));
    print();
  }

  address entry;
  if (is_optimized()) {
    entry = SharedRuntime::get_resolve_opt_virtual_call_stub();
  } else {
    entry = SharedRuntime::get_resolve_virtual_call_stub();
  }

  // A zombie transition will always be safe, since the metadata has already been set to NULL, so
  // we only need to patch the destination
  bool safe_transition = !in_use || is_optimized() || SafepointSynchronize::is_at_safepoint();

  if (safe_transition) {
    // Kill any leftover stub we might have too
    clear_ic_stub();
    if (is_optimized()) {
      set_ic_destination(entry);
    } else {
      set_ic_destination_and_value(entry, (void*)NULL);
    }
  } else {
    // Unsafe transition - create stub.
    InlineCacheBuffer::create_transition_stub(this, NULL, entry);
  }
  // We can't check this anymore. With lazy deopt we could have already
  // cleaned this IC entry before we even return. This is possible if
  // we ran out of space in the inline cache buffer trying to do the
  // set_next and we safepointed to free up space. This is a benign
  // race because the IC entry was complete when we safepointed so
  // cleaning it immediately is harmless.
  // assert(is_clean(), "sanity check");
}


bool CompiledIC::is_clean() const {
  assert (CompiledIC_lock->is_locked() || SafepointSynchronize::is_at_safepoint(), "");
  bool is_clean = false;
  address dest = ic_destination();
  is_clean = dest == SharedRuntime::get_resolve_opt_virtual_call_stub() ||
             dest == SharedRuntime::get_resolve_virtual_call_stub();
  assert(!is_clean || is_optimized() || cached_value() == NULL, "sanity check");
  return is_clean;
}


void CompiledIC::set_to_monomorphic(CompiledICInfo& info) {
  assert (CompiledIC_lock->is_locked() || SafepointSynchronize::is_at_safepoint(), "");
  // Updating a cache to the wrong entry can cause bugs that are very hard
  // to track down - if cache entry gets invalid - we just clean it. In
  // this way it is always the same code path that is responsible for
  // updating and resolving an inline cache
  //
  // The above is no longer true. SharedRuntime::fixup_callers_callsite will change optimized
  // callsites. In addition ic_miss code will update a site to monomorphic if it determines
  // that an monomorphic call to the interpreter can now be monomorphic to compiled code.
  //
  // In both of these cases the only thing being modifed is the jump/call target and these
  // transitions are mt_safe

  Thread *thread = Thread::current();
  if (info.to_interpreter()) {
    // Call to interpreter
    if (info.is_optimized() && is_optimized()) {
       assert(is_clean(), "unsafe IC path");
       MutexLockerEx pl(Patching_lock, Mutex::_no_safepoint_check_flag);
      // the call analysis (callee structure) specifies that the call is optimized
      // (either because of CHA or the static target is final)
      // At code generation time, this call has been emitted as static call
      // Call via stub
      assert(info.cached_metadata() != NULL && info.cached_metadata()->is_method(), "sanity check");
      CompiledStaticCall* csc = compiledStaticCall_at(instruction_address());
      methodHandle method (thread, (Method*)info.cached_metadata());
      csc->set_to_interpreted(method, info.entry());
      if (TraceICs) {
         ResourceMark rm(thread);
         tty->print_cr ("IC@" INTPTR_FORMAT ": monomorphic to interpreter: %s",
           p2i(instruction_address()),
           method->print_value_string());
      }
    } else {
      // Call via method-klass-holder
      InlineCacheBuffer::create_transition_stub(this, info.claim_cached_icholder(), info.entry());
      if (TraceICs) {
         ResourceMark rm(thread);
         tty->print_cr ("IC@" INTPTR_FORMAT ": monomorphic to interpreter via icholder ", p2i(instruction_address()));
      }
    }
  } else {
    // Call to compiled code
    bool static_bound = info.is_optimized() || (info.cached_metadata() == NULL);
#ifdef ASSERT
    CodeBlob* cb = CodeCache::find_blob_unsafe(info.entry());
    assert (cb->is_nmethod(), "must be compiled!");
#endif /* ASSERT */

    // This is MT safe if we come from a clean-cache and go through a
    // non-verified entry point
    bool safe = SafepointSynchronize::is_at_safepoint() ||
                (!is_in_transition_state() && (info.is_optimized() || static_bound || is_clean()));

    if (!safe) {
      InlineCacheBuffer::create_transition_stub(this, info.cached_metadata(), info.entry());
    } else {
      if (is_optimized()) {
      set_ic_destination(info.entry());
      } else {
        set_ic_destination_and_value(info.entry(), info.cached_metadata());
      }
    }

    if (TraceICs) {
      ResourceMark rm(thread);
      assert(info.cached_metadata() == NULL || info.cached_metadata()->is_klass(), "must be");
      tty->print_cr ("IC@" INTPTR_FORMAT ": monomorphic to compiled (rcvr klass) %s: %s",
        p2i(instruction_address()),
        ((Klass*)info.cached_metadata())->print_value_string(),
        (safe) ? "" : "via stub");
    }
  }
  // We can't check this anymore. With lazy deopt we could have already
  // cleaned this IC entry before we even return. This is possible if
  // we ran out of space in the inline cache buffer trying to do the
  // set_next and we safepointed to free up space. This is a benign
  // race because the IC entry was complete when we safepointed so
  // cleaning it immediately is harmless.
  // assert(is_call_to_compiled() || is_call_to_interpreted(), "sanity check");
}


// is_optimized: Compiler has generated an optimized call (i.e., no inline
// cache) static_bound: The call can be static bound (i.e, no need to use
// inline cache)
void CompiledIC::compute_monomorphic_entry(methodHandle method,
                                           KlassHandle receiver_klass,
                                           bool is_optimized,
                                           bool static_bound,
                                           CompiledICInfo& info,
                                           TRAPS) {
  nmethod* method_code = method->code();
  address entry = NULL;
  if (method_code != NULL && method_code->is_in_use()) {
    // Call to compiled code
    if (static_bound || is_optimized) {
      entry      = method_code->verified_entry_point();
    } else {
      entry      = method_code->entry_point();
    }
  }
  if (entry != NULL) {
    // Call to compiled code
    info.set_compiled_entry(entry, (static_bound || is_optimized) ? NULL : receiver_klass(), is_optimized);
  } else {
    // Note: the following problem exists with Compiler1:
    //   - at compile time we may or may not know if the destination is final
    //   - if we know that the destination is final, we will emit an optimized
    //     virtual call (no inline cache), and need a Method* to make a call
    //     to the interpreter
    //   - if we do not know if the destination is final, we emit a standard
    //     virtual call, and use CompiledICHolder to call interpreted code
    //     (no static call stub has been generated)
    //     However in that case we will now notice it is static_bound
    //     and convert the call into what looks to be an optimized
    //     virtual call. This causes problems in verifying the IC because
    //     it look vanilla but is optimized. Code in is_call_to_interpreted
    //     is aware of this and weakens its asserts.

    // static_bound should imply is_optimized -- otherwise we have a
    // performance bug (statically-bindable method is called via
    // dynamically-dispatched call note: the reverse implication isn't
    // necessarily true -- the call may have been optimized based on compiler
    // analysis (static_bound is only based on "final" etc.)
#ifdef COMPILER2
#ifdef TIERED
#if defined(ASSERT)
    // can't check the assert because we don't have the CompiledIC with which to
    // find the address if the call instruction.
    //
    // CodeBlob* cb = find_blob_unsafe(instruction_address());
    // assert(cb->is_compiled_by_c1() || !static_bound || is_optimized, "static_bound should imply is_optimized");
#endif // ASSERT
#else
    assert(!static_bound || is_optimized, "static_bound should imply is_optimized");
#endif // TIERED
#endif // COMPILER2
    if (is_optimized) {
      // Use stub entry
      info.set_interpreter_entry(method()->get_c2i_entry(), method());
    } else {
      // Use icholder entry
      CompiledICHolder* holder = new CompiledICHolder(method(), receiver_klass());
      info.set_icholder_entry(method()->get_c2i_unverified_entry(), holder);
    }
  }
  assert(info.is_optimized() == is_optimized, "must agree");
}


bool CompiledIC::is_icholder_entry(address entry) {
  CodeBlob* cb = CodeCache::find_blob_unsafe(entry);
  if (cb != NULL && cb->is_adapter_blob()) {
    return true;
  }
  // itable stubs also use CompiledICHolder
  if (cb != NULL && cb->is_vtable_blob()) {
    VtableStub* s = VtableStubs::entry_point(entry);
    return (s != NULL) && s->is_itable_stub();
  }

  return false;
}

// ----------------------------------------------------------------------------

void CompiledStaticCall::set_to_clean() {
  assert (CompiledIC_lock->is_locked() || SafepointSynchronize::is_at_safepoint(), "mt unsafe call");
  // Reset call site
  MutexLockerEx pl(SafepointSynchronize::is_at_safepoint() ? NULL : Patching_lock, Mutex::_no_safepoint_check_flag);
#ifdef ASSERT
  CodeBlob* cb = CodeCache::find_blob_unsafe(this);
  assert(cb != NULL && cb->is_nmethod(), "must be nmethod");
#endif
  set_destination_mt_safe(SharedRuntime::get_resolve_static_call_stub());

  // Do not reset stub here:  It is too expensive to call find_stub.
  // Instead, rely on caller (nmethod::clear_inline_caches) to clear
  // both the call and its stub.
}


bool CompiledStaticCall::is_clean() const {
  return destination() == SharedRuntime::get_resolve_static_call_stub();
}

bool CompiledStaticCall::is_call_to_compiled() const {
  return CodeCache::contains(destination());
}


bool CompiledStaticCall::is_call_to_interpreted() const {
  // It is a call to interpreted, if it calls to a stub. Hence, the destination
  // must be in the stub part of the nmethod that contains the call
  nmethod* nm = CodeCache::find_nmethod(instruction_address());
  return nm->stub_contains(destination());
}

void CompiledStaticCall::set(const StaticCallInfo& info) {
  assert (CompiledIC_lock->is_locked() || SafepointSynchronize::is_at_safepoint(), "mt unsafe call");
  MutexLockerEx pl(Patching_lock, Mutex::_no_safepoint_check_flag);
  // Updating a cache to the wrong entry can cause bugs that are very hard
  // to track down - if cache entry gets invalid - we just clean it. In
  // this way it is always the same code path that is responsible for
  // updating and resolving an inline cache
  assert(is_clean(), "do not update a call entry - use clean");

  if (info._to_interpreter) {
    // Call to interpreted code
    set_to_interpreted(info.callee(), info.entry());
  } else {
    if (TraceICs) {
      ResourceMark rm;
      tty->print_cr("CompiledStaticCall@" INTPTR_FORMAT ": set_to_compiled " INTPTR_FORMAT,
                    p2i(instruction_address()),
                    p2i(info.entry()));
    }
    // Call to compiled code
    assert (CodeCache::contains(info.entry()), "wrong entry point");
    set_destination_mt_safe(info.entry());
  }
}


// Compute settings for a CompiledStaticCall. Since we might have to set
// the stub when calling to the interpreter, we need to return arguments.
void CompiledStaticCall::compute_entry(methodHandle m, StaticCallInfo& info) {
  nmethod* m_code = m->code();
  info._callee = m;
  if (m_code != NULL && m_code->is_in_use()) {
    info._to_interpreter = false;
    info._entry  = m_code->verified_entry_point();
  } else {
    // Callee is interpreted code.  In any case entering the interpreter
    // puts a converter-frame on the stack to save arguments.
    assert(!m->is_method_handle_intrinsic(), "Compiled code should never call interpreter MH intrinsics");
    info._to_interpreter = true;
    info._entry      = m()->get_c2i_entry();
  }
}

address CompiledStaticCall::find_stub() {
  // Find reloc. information containing this call-site
  RelocIterator iter((nmethod*)NULL, instruction_address());
  while (iter.next()) {
    if (iter.addr() == instruction_address()) {
      switch(iter.type()) {
        case relocInfo::static_call_type:
          return iter.static_call_reloc()->static_stub();
        // We check here for opt_virtual_call_type, since we reuse the code
        // from the CompiledIC implementation
        case relocInfo::opt_virtual_call_type:
          return iter.opt_virtual_call_reloc()->static_stub();
        case relocInfo::poll_type:
        case relocInfo::poll_return_type: // A safepoint can't overlap a call.
        default:
          ShouldNotReachHere();
      }
    }
  }
  return NULL;
}


//-----------------------------------------------------------------------------
// Non-product mode code
#ifndef PRODUCT

void CompiledIC::verify() {
  // make sure code pattern is actually a call imm32 instruction
  _ic_call->verify();
  if (os::is_MP()) {
    _ic_call->verify_alignment();
  }
  assert(is_clean() || is_call_to_compiled() || is_call_to_interpreted()
          || is_optimized() || is_megamorphic(), "sanity check");
}

void CompiledIC::print() {
  print_compiled_ic();
  tty->cr();
}

void CompiledIC::print_compiled_ic() {
  tty->print("Inline cache at " INTPTR_FORMAT ", calling %s " INTPTR_FORMAT " cached_value " INTPTR_FORMAT,
             p2i(instruction_address()), is_call_to_interpreted() ? "interpreted " : "", p2i(ic_destination()), p2i(is_optimized() ? NULL : cached_value()));
}

void CompiledStaticCall::print() {
  tty->print("static call at " INTPTR_FORMAT " -> ", p2i(instruction_address()));
  if (is_clean()) {
    tty->print("clean");
  } else if (is_call_to_compiled()) {
    tty->print("compiled");
  } else if (is_call_to_interpreted()) {
    tty->print("interpreted");
  }
  tty->cr();
}

#endif // !PRODUCT
