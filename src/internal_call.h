#pragma once
// Thread-local guard (spec 6.1): while set, our DXGI exports and factory hooks
// are pure pass-throughs, because our own DXGI/D3D12 calls can re-enter us
// through ReShade, or directly when the bridge is the process's dxgi.dll.

namespace acdb {

bool IsInternalCall();

class InternalCallScope {
public:
    InternalCallScope();
    ~InternalCallScope();
    InternalCallScope(const InternalCallScope&) = delete;
    InternalCallScope& operator=(const InternalCallScope&) = delete;

private:
    bool prev_;
};

}  // namespace acdb
