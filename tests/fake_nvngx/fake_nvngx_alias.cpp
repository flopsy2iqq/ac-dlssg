// A fake NGX module whose CreateFeature and EvaluateFeature exports resolve to
// one address (through the .def below). NgxHook must skip it as a placeholder
// (spec 6.5: two exports sharing one address).
extern "C" int __cdecl FakeNgxAliasedEntry(void*, unsigned int, void*, void**) { return 0x1; }
