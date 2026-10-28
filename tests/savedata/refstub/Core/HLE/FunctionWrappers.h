// Test-only stand-in for PPSSPP's Core/HLE/FunctionWrappers.h: the syscall wrappers are never
// called by the test, they only have to exist.
#pragma once
template <auto F> void WrapI_UI() {}
template <auto F> void WrapI_UUI() {}
template <auto F> void WrapI_UUU() {}
template <auto F> void WrapI_UIIUU() {}
template <auto F> void WrapI_U() {}
template <auto F> void WrapU_UIUII() {}
template <auto F> void WrapI_UIUII() {}
