#include "internal_call.h"

namespace acdb {
namespace {

thread_local bool t_internal_call = false;

}  // namespace

bool IsInternalCall() { return t_internal_call; }

InternalCallScope::InternalCallScope() : prev_(t_internal_call) { t_internal_call = true; }

InternalCallScope::~InternalCallScope() { t_internal_call = prev_; }

}  // namespace acdb
