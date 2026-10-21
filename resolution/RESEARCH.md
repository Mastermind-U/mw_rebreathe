# Native Video options resolution support

The installed English Steam executable is Morrowind 1.6.1820, PE32, preferred
image base `0x00400000`, SHA-256
`eac72e1b3524ee0012d080d3c2e2e00e401464e41ee54f9afd6aaaadf2c221f5`.
Addresses below are virtual addresses in this executable; subtract `0x400000`
for their RVAs and raw file offsets in its `.text` section.

`MenuVideo` is registered at `0x637080`. Its construction function starts at
`0x6372A0`; the call at `0x6375C4` obtains the supported resolution strings from
`0x429D30`. That function obtains renderer modes and calls `0x429F90` to filter
and format them.

The restriction is an explicit 4:3 test at `0x42A07C`:

```asm
0042A07C mov   ecx,[esi+4]          ; height
0042A07F mov   edx,[esi]            ; width
0042A089 fild  qword ptr [esp+34h] ; height, zero-extended
0042A095 fidiv dword ptr [esp+3Ch] ; height / width
0042A099 fcomp dword ptr [746920h] ; 0.75f
0042A09F fnstsw ax
0042A0A1 test  ah,40h
0042A0A4 je    0042A1BA            ; skip every non-4:3 mode
```

The six bytes at `0x42A0A4` are `0F 84 10 01 00 00`. Replacing these bytes with
six `90` bytes removes the aspect-ratio exclusion. A second change redirects
the five-byte Video-list sorting call at `0x429DEB` to the bounded two-field
sorter described below. `resolution.cpp` validates the surrounding instructions,
the float constant, PE architecture, preferred image base, image size and
original branch/call before changing memory. A mismatch skips both changes.
Calling it again is safe. The patch does not modify the game file.

The native filters before this branch still require 32-bit color, width >=640,
height >=480, and renderer support. No unsupported fullscreen modes are
invented. A resolution appears if the existing Direct3D 8 renderer advertises
it. Therefore 1920x1080, 2560x1440 or 3840x2160 availability depends on the
graphics driver and connected monitor.

Selecting a native list row calls `0x638350`, asks the game's confirmation
question, then invokes `0x6383E0`. The latter parses the selected width/height
at `0x63843E`, stores them in the existing renderer settings at `0x638488` /
`0x638494`, and calls the original resize/recreation path at `0x6384D1`
(`0x41A700`). This is the real in-game settings list and native renderer path,
not merely a registry override. Visual and gameplay validation is still
required before claiming every ultrawide mode works correctly.

The original Morrowind Launcher is a separate executable; this module targets
the game's Video options and does not patch the original launcher dialog.

## Native Video-list sorting crash

The first implementation removed only the aspect-ratio exclusion. It was
incomplete: a later real-game test crashed when opening the Video tab. The
captured minidump identifies a read from address `0x2` in `msvcrt.dll+0x79168`,
called from `0x42C7E5` (return address `0x42C7E7`). The Video construction stack
contains `0x6375C9`, `0x429DF0`, `0x42BD37` and this native partition sorter.

The mode producer formats strings using `0x78563C`, **`%d x %d`**. The native
partition at `0x42C6F0` parses them with `0x785644`, **`%u x %u x %u`** and then
compares all three outputs. There is no third field in a Video resolution.
`sscanf` leaves that output unchanged, and the native comparator reads
uninitialized stack values. In the captured dump, third-field slots contain
iterator pointers `0x2631F048` / `0x2631F1F8`; another comparison consumes `0`
and `0x42BD2D`, a return address. The partition consequently walks before the
range, obtains `[EDI+4]=0x2`, and passes that value as a string pointer to CRT
`sscanf`. Merely appending a third number is insufficient: the partition's
tie-handling assumptions also need to be avoided.

The fix replaces **only** the call at `0x429DEB`. Original bytes are
`E8 A0 1D 00 00`, calling `0x42BB90`. Four arguments are pushed by the native
caller, which removes 16 bytes at `0x429DF0`; the replacement has the same
`__cdecl` call contract. The first two arguments delimit a contiguous array
of 16-byte MSVCP60 `basic_string<char>` objects. The final two arguments are
unused by the replacement.

The replacement bounds the array, parses exactly width and height, and applies
a strict numeric ordering `(width, height)`. It permutes complete 16-byte rows
as trivial storage. It never constructs, copies with a modern string API, or
destroys foreign MSVCP60 strings. These legacy strings contain a data pointer,
length and capacity rather than an inline buffer, so relocation preserves
ownership. Malformed rows compare after valid resolutions. The adapter-dialog
sorter, global CRT functions and all other callers remain unchanged.

Two native regression executables validate this change:

- `test_resolution.cpp` maps the installed executable as data. It checks both
  guarded changes, the replacement CALL target, rejection of unexpected code,
  restored page protection and idempotence. Only six filter bytes and five
  sorting-call bytes change; all 11 checks passed.
- `test_mode_sort.cpp` creates genuine strings through system `MSVCP60.dll`,
  including its native copy constructor and destructor. All 27 admitted driver
  dimensions plus shared duplicates are shuffled 256 times and sorted through
  the production wrapper. It checks ordering, preservation of every complete
  row, pointer and native reference count, bounded invalid ranges and rejection
  of the captured pointer `0x2`. All eight checks passed and native destruction
  completed successfully. Compile this test source alone because it includes
  the production implementation in its translation unit.

## Evidence and related primary sources

Local evidence: MSVC `dumpbin /DISASM:NOBYTES` ranges
`0x637080,0x637710`, `0x429D30,0x42A1E5`, `0x638350,0x638576`,
`0x42BCE0,0x42BD60`, `0x42C6F0,0x42C840`, and the captured
`modernization/logs/game.log.dmp` from the Video-tab failure.
No third-party implementation code is copied by this module.

For comparison, MGE XE also exposes arbitrary windowed resolutions through
its external setup utility. Its own strings explicitly distinguish windowed
arbitrary resolutions from fullscreen monitor modes:
[MGE XE Template.lng](https://github.com/Hrnchamd/MGE-XE/blob/master/assets/Template.lng).
This project uses the local executable's existing native mode selection code.

## Resource handle reuse regression

`test_cache_handle_reuse.cpp` separately tests the production RAM-read adapters
without injecting a process or launching the game. Run `test-cache-build.cmd`
with Visual Studio 2022 C++ tools installed. It creates and removes two small
temporary files in its current workspace directory.

The old `HANDLE -> CacheEntry*` map could outlive a file object closed by an
unhooked module. Windows immediately reused that numeric handle for another
file in the regression. Its intercepted open was ineligible for caching, so
the old map entry survived and `CacheReadFile` returned resource A's bytes for
file B: `reused=1 cache_hit=1 expected=B actual=A`. This reproduces a concrete
wrong-resource-data defect; it does not establish the cause of a separate
native heap failure observed during game shutdown.

The fix invalidates an old association on every successful intercepted open,
before checking creation mode, access, flags or path. Cached reads additionally
validate the live volume serial number, file ID, size, creation time and last
write time. A mismatch removes the association and delegates to the operating
system. No duplicate handles are retained and file lifetimes are unchanged.
Normal cache statistics include `stale_handles` so real gameplay can show
whether stale entries were rejected.

All four regression scenarios passed with real numeric-handle reuse: an
intercepted noncached open, an intercepted `OPEN_ALWAYS`, an entirely unhooked
reopen, and an intercepted write-only open that must retain the native access
error. Both fixtures have identical size and timestamps, so the unhooked-open
case specifically exercises file-ID validation. Before each reuse, a normal
read of A must still be served from RAM. Compile-time assertions also verify
that the MMIO `_lread`, `_llseek` and `_lclose` replacements have the exact
callback types declared in the Windows SDK.

