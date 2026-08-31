# MX Bikes — OpenGL matrix pipeline / camera override analysis

Target: `/Users/seandahan/Downloads/mxbikes.exe.unpacked.exe` (PE x64, Steamless-unpacked)
Image base `0x140000000`. **RVA = VA − 0x140000000.**
Sections: `.text` 0x1000..0x3207B3, `.rdata` 0x321000, `.data` 0x37C000, `.pdata` 0x10A4000.

Every finding is labelled **CONFIRMED** (read directly out of the disassembly / import table),
**INFERRED** (strong structural deduction, not literally proven) or **UNKNOWN**.

---

## 0. Executive summary

* MX Bikes drives the camera through the classic fixed-function matrix stack. **CONFIRMED**
* **`glPushMatrix` / `glPopMatrix` / `glTranslatef` / `glRotatef` / `glScalef` are NOT imported at all.**
  Matrix state is only ever changed by `glLoadIdentity`, `glLoadMatrixf`, `glMultMatrixf`,
  `glFrustum`, `glOrtho`. A hook therefore needs **one** "current mode" variable — no stack. **CONFIRMED**
* **`glMultMatrixf` is used on `GL_PROJECTION` only — never on `GL_MODELVIEW`.** All 5 sites sit
  immediately after `glFrustum`/`glOrtho`. So *every* modelview change in the whole game is a
  `glLoadIdentity` or a `glLoadMatrixf`. **CONFIRMED**
* The **main scene view matrix** reaches the GPU at **`glLoadMatrixf` VA `0x14024FF3B` / RVA `0x24FF3B`**
  (inside `RenderView`, root `0x14024F740`). A second, equivalent load exists at
  **VA `0x14024F1F1` / RVA `0x24F1F1`** (inside `SetupSceneCamera`, root `0x14024EF80`). **CONFIRMED**
* Both are `transpose()` of a 4×4 row-major float matrix living at **`viewState + 0x20`**, where
  `viewState = *(void**)(viewDesc + 0x30)`. **CONFIRMED**
* `viewState+0x20` is produced by `Mat4InverseRigid(viewState+0x20, viewState+0x60)` at
  **VA `0x140249555` / RVA `0x249555`** — i.e. **view = inverse(camera world transform)**, and the
  camera world transform lives at **`viewState + 0x60`**, a 4×4 row-major rigid matrix
  (**3×3 basis + translation**, *not* euler, *not* quaternion). **CONFIRMED**
* `viewState` objects come from a **static global pool at VA `0x14057C0A0` / RVA `0x57C0A0`,
  12 slots × `0xE4` bytes**. **CONFIRMED**
* The game *does* use ARB/GLSL shaders, **but every shader uses `ftransform()` /
  `gl_ModelViewMatrix` / `gl_NormalMatrix`** — the fixed-function matrices. A `glLoadMatrixf`
  hook therefore covers the shader path too. **CONFIRMED** (GLSL source is embedded in the exe)
* The engine's view matrix is **+Z-forward**. The GL −Z convention is applied by post-multiplying
  the projection by `diag(1,1,-1,1)`. This is the single strongest runtime marker for
  "a 3D scene pass is starting". **CONFIRMED**

**Verdict on the API-boundary strategy: it works, and there is an exact, offset-free discriminator.**
See §6.

---

## 1. IAT slots and every call site

IAT slots (OPENGL32.dll, 70 imports total, IAT block `0x1403215A0`..`0x1403217C8`):

| import | slot VA | slot RVA | call sites |
|---|---|---|---|
| `glViewport`      | `0x140321638` | `0x321638` | 24 |
| `glGetFloatv`     | `0x140321618` | `0x321618` | 1  |
| `glMultMatrixf`   | `0x140321700` | `0x321700` | 5  |
| `glOrtho`         | `0x140321708` | `0x321708` | 5  |
| `glFrustum`       | `0x140321728` | `0x321728` | 4  |
| `glLoadMatrixf`   | `0x140321790` | `0x321790` | 21 |
| `glMatrixMode`    | `0x140321798` | `0x321798` | 72 |
| `glLoadIdentity`  | `0x1403217C8` | `0x3217C8` | 52 |

(GDI32 `SwapBuffers` slot `0x140321070` / RVA `0x321070`, exactly **1** call site — VA `0x14025A8B6`.) **CONFIRMED**

> Note on method: `R.xrefs()` on the IAT slots under-reports badly (it found 2/72 `glMatrixMode`
> sites). The numbers below come from a linear byte scan for `FF 15 disp32` / `FF 25 disp32`
> resolving exactly into an IAT slot. All sites are `call qword ptr [rip+disp]` (`FF 15`); there
> are **no** jump thunks for these imports.

`.pdata` in this binary is heavily chunked (chained UNWIND_INFO), so below each site is given with
both its **.pdata chunk** and its **unwind root** (the real function entry point).

### glMatrixMode — 72 sites (mode histogram: PROJECTION 27, MODELVIEW 41, TEXTURE 4)

```
VA           RVA       chunk     root                 mode
0x140209C87  0x209C87  0x209BC0  0x140209BC0          PROJECTION
0x140209C98  0x209C98  0x209BC0  0x140209BC0          MODELVIEW
0x140209D89  0x209D89  0x209D40  0x140209D40          PROJECTION
0x140209D9A  0x209D9A  0x209D40  0x140209D40          MODELVIEW
0x140227CAB  0x227CAB  0x227B25  0x140227B00          MODELVIEW
0x14022F409  0x22F409  0x22F260  0x14022F260          PROJECTION
0x14022F41A  0x22F41A  0x22F260  0x14022F260          MODELVIEW
0x1402300A0  0x2300A0  0x22FFB0  0x14022FFB0          MODELVIEW
0x1402300B1  0x2300B1  0x22FFB0  0x14022FFB0          PROJECTION
0x140232519  0x232519  0x232470  0x140232470          TEXTURE
0x140235673  0x235673  0x235550  0x140235550          TEXTURE
0x140238D84  0x238D84  0x238D03  0x140238CF0          TEXTURE
0x140239157  0x239157  0x238DA5  0x140238CF0          TEXTURE
0x14023939F  0x23939F  0x2392C7  0x140239290          PROJECTION
0x1402393B0  0x2393B0  0x2392C7  0x140239290          MODELVIEW
0x140239550  0x239550  0x2392C7  0x140239290          PROJECTION
0x140239561  0x239561  0x2392C7  0x140239290          MODELVIEW
0x14023974F  0x23974F  0x2392C7  0x140239290          PROJECTION
0x140239760  0x239760  0x2392C7  0x140239290          MODELVIEW
0x140239943  0x239943  0x23986B  0x140239290          PROJECTION
0x140239954  0x239954  0x23986B  0x140239290          MODELVIEW
0x140239ADB  0x239ADB  0x239979  0x140239290          PROJECTION
0x140239AEC  0x239AEC  0x239979  0x140239290          MODELVIEW
0x140239C46  0x239C46  0x239979  0x140239290          PROJECTION
0x140239C57  0x239C57  0x239979  0x140239290          MODELVIEW
0x140239EEA  0x239EEA  0x239DF2  0x140239DB0          PROJECTION
0x140239EFB  0x239EFB  0x239DF2  0x140239DB0          MODELVIEW
0x14023A0D6  0x23A0D6  0x239DF2  0x140239DB0          PROJECTION
0x14023A0E7  0x23A0E7  0x239DF2  0x140239DB0          MODELVIEW
0x14023CAC4  0x23CAC4  0x23CA8C  0x14023CA30          PROJECTION
0x14023CAD5  0x23CAD5  0x23CA8C  0x14023CA30          MODELVIEW
0x14023D4DA  0x23D4DA  0x23D4A0  0x14023D4A0          PROJECTION
0x14023D4EB  0x23D4EB  0x23D4A0  0x14023D4A0          MODELVIEW
0x14023D76A  0x23D76A  0x23D730  0x14023D730          PROJECTION
0x14023D77B  0x23D77B  0x23D730  0x14023D730          MODELVIEW
0x14023DA1A  0x23DA1A  0x23D9E0  0x14023D9E0          PROJECTION
0x14023DA2B  0x23DA2B  0x23D9E0  0x14023D9E0          MODELVIEW
0x14023DD0B  0x23DD0B  0x23DC40  0x14023DC40          PROJECTION
0x14023DD1C  0x23DD1C  0x23DC40  0x14023DC40          MODELVIEW
0x14023E069  0x23E069  0x23DFA8  0x14023DF40          PROJECTION
0x14023E07A  0x23E07A  0x23DFA8  0x14023DF40          MODELVIEW
0x140240A75  0x240A75  0x2409D0  0x1402409D0          MODELVIEW
0x140245FBC  0x245FBC  0x245E05  0x140245CB0          PROJECTION
0x140246175  0x246175  0x24605F  0x140245CB0          MODELVIEW
0x1402462F7  0x2462F7  0x24629C  0x140245CB0          PROJECTION
0x140246308  0x246308  0x24629C  0x140245CB0          MODELVIEW
0x140246FD1  0x246FD1  0x246F40  0x140246F40          MODELVIEW
0x14024B46A  0x24B46A  0x24B430  0x14024B430          MODELVIEW
0x14024EAEB  0x24EAEB  0x24EA47  0x14024E740          MODELVIEW
0x14024EC30  0x24EC30  0x24EA47  0x14024E740          MODELVIEW
0x14024EC69  0x24EC69  0x24EA47  0x14024E740          MODELVIEW
0x14024EC97  0x24EC97  0x24EA47  0x14024E740          MODELVIEW
0x14024F099  0x24F099  0x24EFAF  0x14024EF80          PROJECTION
0x14024F1E3  0x24F1E3  0x24EFAF  0x14024EF80          MODELVIEW   <-- precedes VIEW load
0x14024F4C8  0x24F4C8  0x24F43C  0x14024EF80          MODELVIEW
0x14024F9F9  0x24F9F9  0x24F76C  0x14024F740          PROJECTION
0x14024FC7A  0x24FC7A  0x24F76C  0x14024F740          PROJECTION
0x14024FC8B  0x24FC8B  0x24F76C  0x14024F740          MODELVIEW
0x14024FE21  0x24FE21  0x24F76C  0x14024F740          PROJECTION
0x14024FF2D  0x24FF2D  0x24F76C  0x14024F740          MODELVIEW   <-- precedes MAIN VIEW load
0x14025049A  0x25049A  0x24F76C  0x14024F740          PROJECTION
0x1402504AB  0x2504AB  0x24F76C  0x14024F740          MODELVIEW
0x140250823  0x250823  0x2507E0  0x14024F740          MODELVIEW
0x1402558B5  0x2558B5  0x2558A5  0x140254E40          PROJECTION
0x140255A0C  0x255A0C  0x2558A5  0x140254E40          MODELVIEW
0x14025BFBD  0x25BFBD  0x25BD92  0x14025BD60          MODELVIEW
0x14025E299  0x25E299  0x25E070  0x14025DEA0          MODELVIEW
0x1402612FE  0x2612FE  0x261220  0x140261220          MODELVIEW
0x140262DCD  0x262DCD  0x262DB0  0x140262DB0          MODELVIEW
0x1402649FE  0x2649FE  0x264858  0x1402646B0          MODELVIEW
0x140265407  0x265407  0x2653C4  0x140264AF0          PROJECTION
0x140265418  0x265418  0x2653C4  0x140264AF0          MODELVIEW
```

### glLoadMatrixf — 21 sites

```
VA           RVA       chunk     root          classification
0x140227CB6  0x227CB6  0x227B25  0x140227B00   MODELVIEW  sky/backdrop: view * translate(camPos)
0x140232524  0x232524  0x232470  0x140232470   TEXTURE    projected-texture matrix
0x140238D8F  0x238D8F  0x238D03  0x140238CF0   TEXTURE    texture matrix
0x140240A83  0x240A83  0x2409D0  0x1402409D0   MODELVIEW  per-object  (Mat4Mul with rbx+0x20)
0x140246180  0x246180  0x24605F  0x140245CB0   MODELVIEW  per-object  (array stride 0x3F8)
0x140246FDF  0x246FDF  0x246F40  0x140246F40   MODELVIEW  per-object  (r13+0x20)
0x14024B473  0x24B473  0x24B430  0x14024B430   MODELVIEW  helper: load caller-supplied matrix (rdx)
0x14024EAF9  0x24EAF9  0x24EA47  0x14024E740   MODELVIEW  DrawObject: restore view ptr [rsp+0x188]
0x14024EC3E  0x24EC3E  0x24EA47  0x14024E740   MODELVIEW  DrawObject: restore view ptr
0x14024EC77  0x24EC77  0x24EA47  0x14024E740   MODELVIEW  DrawObject: transpose(obj+0x140)
0x14024ECA5  0x24ECA5  0x24EA47  0x14024E740   MODELVIEW  DrawObject: restore view ptr
0x14024F1F1  0x24F1F1  0x24EFAF  0x14024EF80   MODELVIEW  *** SCENE VIEW MATRIX (variant B) ***
0x14024F4D6  0x24F4D6  0x24F43C  0x14024EF80   MODELVIEW  re-load of the same view matrix
0x14024FF3B  0x24FF3B  0x24F76C  0x14024F740   MODELVIEW  *** MAIN SCENE VIEW MATRIX ***
0x14025082E  0x25082E  0x2507E0  0x14024F740   MODELVIEW  view*obj for the debug wireframe AABB
0x140255A1A  0x255A1A  0x2558A5  0x140254E40   MODELVIEW  per-object (frame-end textured quad pass)
0x14025BFCB  0x25BFCB  0x25BD92  0x14025BD60   MODELVIEW  per-object (rbx+0x20)
0x14025E2A7  0x25E2A7  0x25E070  0x14025DEA0   MODELVIEW  per-object (r14)
0x140261307  0x261307  0x261220  0x140261220   MODELVIEW  particle/billboard (r15+0x288)
0x140262DD6  0x262DD6  0x262DB0  0x140262DB0   MODELVIEW  helper: load caller-supplied matrix (r8)
0x140264A0C  0x264A0C  0x264858  0x1402646B0   MODELVIEW  per-object (r14+0x20)
```

### glMultMatrixf — 5 sites (ALL in GL_PROJECTION)

```
0x140246018  0x246018  0x245E05  0x140245CB0   after glOrtho  (2D/HUD ortho pass)
0x14024F188  0x24F188  0x24EFAF  0x14024EF80   after glFrustum/glOrtho — Z-flip diag(1,1,-1,1)
0x14024FAE4  0x24FAE4  0x24F76C  0x14024F740   after glFrustum/glOrtho — Z-flip (sky/backdrop pass)
0x14024FF0D  0x24FF0D  0x24F76C  0x14024F740   after glFrustum/glOrtho — Z-flip (MAIN scene pass)
0x1402559A3  0x2559A3  0x2558A5  0x140254E40   after glFrustum/glOrtho — Z-flip (frame-end pass)
```

### glFrustum — 4 sites

```
0x14024F17A  0x24F17A  0x24EFAF  0x14024EF80
0x14024FAD9  0x24FAD9  0x24F76C  0x14024F740   (sky / backdrop pass)
0x14024FF02  0x24FF02  0x24F76C  0x14024F740   (MAIN scene pass)
0x140255995  0x255995  0x2558A5  0x140254E40
```

### glOrtho — 5 sites

```
0x14024600D  0x24600D  0x245E05  0x140245CB0   (2D / HUD)
0x14024F136  0x24F136  0x24EFAF  0x14024EF80   (ortho branch of the same camera setup)
0x14024FA95  0x24FA95  0x24F76C  0x14024F740
0x14024FEBE  0x24FEBE  0x24F76C  0x14024F740
0x140255951  0x255951  0x2558A5  0x140254E40
```

### glViewport — 24 sites

```
0x14022F430 0x22F430 0x22F260 0x14022F260    GL device/state reset
0x140230005 0x230005 0x22FFB0 0x14022FFB0    GL device/state reset
0x140239394 0x239394 0x2392C7 0x140239290    post-process / GLSL fullscreen pass
0x140239545 0x239545 0x2392C7 0x140239290    post-process
0x140239744 0x239744 0x2392C7 0x140239290    post-process
0x140239938 0x239938 0x23986B 0x140239290    post-process
0x140239AD0 0x239AD0 0x239979 0x140239290    post-process
0x140239C3B 0x239C3B 0x239979 0x140239290    post-process
0x140239EDF 0x239EDF 0x239DF2 0x140239DB0    post-process
0x14023A0CB 0x23A0CB 0x239DF2 0x140239DB0    post-process
0x140245FB1 0x245FB1 0x245E05 0x140245CB0    2D / HUD ortho pass
0x1402462EC 0x2462EC 0x24629C 0x140245CB0    2D
0x14024F0EB 0x24F0EB 0x24EFAF 0x14024EF80    scene camera (variant B)
0x14024FA4B 0x24FA4B 0x24F76C 0x14024F740    scene camera (sky/backdrop pass)
0x14024FE73 0x24FE73 0x24F76C 0x14024F740    scene camera (MAIN pass)
0x14025048F 0x25048F 0x24F76C 0x14024F740    2D restore at end of RenderView
0x1402526B3 0x2526B3 0x252510 0x140252510
0x1402526FC 0x2526FC 0x252510 0x140252510
0x140252AFD 0x252AFD 0x252510 0x140252510
0x140254E25 0x254E25 0x254DBF 0x140252BB0
0x140255906 0x255906 0x2558A5 0x140254E40
0x140255BE0 0x255BE0 0x2558A5 0x140254E40
0x1402653FC 0x2653FC 0x2653C4 0x140264AF0    screenshot (glReadPixels) path
0x14026554B 0x26554B 0x2653C4 0x140264AF0    screenshot restore
```

### glLoadIdentity — 52 sites

```
0x140209C8D 0x209C8D  root 0x140209BC0      0x140209C9E 0x209C9E  root 0x140209BC0
0x140209D8F 0x209D8F  root 0x140209D40      0x140209DA0 0x209DA0  root 0x140209D40
0x14022F40F 0x22F40F  root 0x14022F260      0x14022F420 0x22F420  root 0x14022F260
0x1402300A6 0x2300A6  root 0x14022FFB0      0x1402300B7 0x2300B7  root 0x14022FFB0
0x140232547 0x232547  root 0x140232470      0x140235679 0x235679  root 0x140235550
0x14023915D 0x23915D  root 0x140238CF0      0x1402393A5 0x2393A5  root 0x140239290
0x1402393B6 0x2393B6  root 0x140239290      0x140239556 0x239556  root 0x140239290
0x140239567 0x239567  root 0x140239290      0x140239755 0x239755  root 0x140239290
0x140239766 0x239766  root 0x140239290      0x140239949 0x239949  root 0x140239290
0x14023995A 0x23995A  root 0x140239290      0x140239AE1 0x239AE1  root 0x140239290
0x140239AF2 0x239AF2  root 0x140239290      0x140239C4C 0x239C4C  root 0x140239290
0x140239C5D 0x239C5D  root 0x140239290      0x140239EF0 0x239EF0  root 0x140239DB0
0x140239F01 0x239F01  root 0x140239DB0      0x14023A0DC 0x23A0DC  root 0x140239DB0
0x14023A0ED 0x23A0ED  root 0x140239DB0      0x14023CACA 0x23CACA  root 0x14023CA30
0x14023CADB 0x23CADB  root 0x14023CA30      0x14023D4E0 0x23D4E0  root 0x14023D4A0
0x14023D4F1 0x23D4F1  root 0x14023D4A0      0x14023D770 0x23D770  root 0x14023D730
0x14023D781 0x23D781  root 0x14023D730      0x14023DA20 0x23DA20  root 0x14023D9E0
0x14023DA31 0x23DA31  root 0x14023D9E0      0x14023DD11 0x23DD11  root 0x14023DC40
0x14023DD22 0x23DD22  root 0x14023DC40      0x14023E06F 0x23E06F  root 0x14023DF40
0x14023E080 0x23E080  root 0x14023DF40      0x140245FC2 0x245FC2  root 0x140245CB0
0x1402462FD 0x2462FD  root 0x140245CB0      0x14024630E 0x24630E  root 0x140245CB0
0x14024F09F 0x24F09F  root 0x14024EF80      0x14024F9FF 0x24F9FF  root 0x14024F740
0x14024FC80 0x24FC80  root 0x14024F740      0x14024FC91 0x24FC91  root 0x14024F740
0x14024FE27 0x24FE27  root 0x14024F740      0x1402504A0 0x2504A0  root 0x14024F740
0x1402504B1 0x2504B1  root 0x14024F740      0x1402558BB 0x2558BB  root 0x140254E40
0x14026540D 0x26540D  root 0x140264AF0      0x14026541E 0x26541E  root 0x140264AF0
```

---

## 2. Which `glLoadMatrixf` is the main scene view — and why

### 2.1 The matrix helper library (all CONFIRMED)

| VA | meaning |
|---|---|
| `0x140266390` | `Mat4Identity(float* m)` — writes `1.0f` at `m[0], m[5], m[10], m[15]`, 0 elsewhere |
| `0x1402663E0` | `Mat4Mul(float* dst, const float* A, const float* B)` — 4×4, aliasing-safe via a stack temp |
| `0x1402665F0` | `Mat4Copy(float* dst, const float* src)` — 64 bytes |
| `0x1402666D0` | `Mat4Transpose(float* dst, const float* src)` |
| `0x140266750` | `Mat4InverseRigid(float* dst, const float* src)` — negates `src[3],src[7],src[11]`, transposes, then rebuilds the translation column as `-Rᵀ·t`, sets `dst[12..14]=0, dst[15]=1` |
| `0x140266E20` | transform point by matrix (INFERRED) |

The engine stores matrices **row-major** (`S[i][j] = s[4i+j]`, translation at `s[3], s[7], s[11]`).
`Mat4Transpose` converts to GL's column-major layout, so the matrix handed to `glLoadMatrixf`
has the standard GL layout with **translation at `m[12], m[13], m[14]`**. **CONFIRMED**

### 2.2 The decisive code — `RenderView` (root `0x14024F740`)

`RenderView(viewDesc /*rcx*/, flags /*edx*/, int /*r8d*/, float /*xmm3*/)`.
`rbp = viewDesc` throughout.

```
0x14024F77F  call 0x140249510              ; Camera::Update(viewDesc)   <-- rebuilds view from world
...
;   PASS 1  (sky / backdrop) — projection only, NO modelview view load
0x14024F9F9  glMatrixMode(GL_PROJECTION)
0x14024F9FF  glLoadIdentity
0x14024FA4B  glViewport(...)
0x14024FA95  glOrtho | 0x14024FAD9 glFrustum
0x14024FAE4  glMultMatrixf(&stack)         ; diag(1,1,-1,1)
0x14024FB00  glCullFace(GL_FRONT)          ; <-- next GL call is NOT a modelview load
...          sub_14024B430 / sub_140262DB0 load their own modelviews

;   PASS 2  (2D overlay)
0x14024FC7A  glMatrixMode(GL_PROJECTION); 0x14024FC80 glLoadIdentity
0x14024FC8B  glMatrixMode(GL_MODELVIEW);  0x14024FC91 glLoadIdentity

;   PASS 3  (MAIN 3D SCENE) — inside a loop over scene layers
0x14024FE21  glMatrixMode(GL_PROJECTION)
0x14024FE27  glLoadIdentity
0x14024FE73  glViewport(W*[rbp+0x1C], H*[rbp+0x24], W*[rbp+0x14], H*[rbp+0x18])
0x14024FEBE  glOrtho    (if [rbp+0xC4] != 0)
0x14024FF02  glFrustum  (else)   l=[rbp+0x48] r=[rbp+0x50] b=[rbp+0x54] t=[rbp+0x4C]
                                 n=[rbp+0x6C] f=[rbp+0x70]
0x14024FF0D  glMultMatrixf(&stack[0x50])   ; diag(1,1,-1,1)   Z-FLIP
0x14024FF13  mov  rdx, [rbp+0x30]          ; viewState
0x14024FF1F  add  rdx, 0x20                ; &viewState->viewMatrix
0x14024FF23  call Mat4Transpose(&stack[0xD0], rdx)
0x14024FF2D  glMatrixMode(GL_MODELVIEW)
0x14024FF3B  glLoadMatrixf(&stack[0xD0])   ; *** THE MAIN SCENE VIEW MATRIX ***
...
0x140250312  call 0x14024F740              ; RECURSION -> mirror / RTT sub-view (flags=0x1B)
0x14025048F  glViewport + PROJ/MODELVIEW identity   ; 2D restore
0x1402507B9  call 0x14024EF80              ; SetupSceneCamera (aux)
0x14025082E  glLoadMatrixf(view*obj)        ; debug wireframe AABB (glBegin GL_LINES, 24 verts)
```

**Why `0x24FF3B` and not any of the other 18 MODELVIEW loads:**

1. **It is the only `glLoadMatrixf` in the binary whose argument is `transpose(viewState+0x20)`
   with nothing multiplied in.** Every other MODELVIEW load goes through
   `Mat4Mul(dst, view_or_identity, objectMatrix)` first (offsets `+0x20` of an *object*,
   `+0x140` of an object, `+0x288`, stride `0x3F8`, …). **CONFIRMED**
2. **It is preceded by `glFrustum` + the `diag(1,1,-1,1)` Z-flip in `GL_PROJECTION`, with
   `glMatrixMode(GL_MODELVIEW)` as the very next GL call.** The 2D/HUD passes never touch
   `glFrustum`; the post-process passes use identity for both matrices. **CONFIRMED**
3. **It is the `glLoadMatrixf` that the whole per-layer draw loop `0x24FD51..0x24FF80` re-issues
   at the top of every iteration**, i.e. it is the pass-level camera, not an object transform.
   **CONFIRMED**
4. `0x24F1F1` (in `SetupSceneCamera`, root `0x14024EF80`) is functionally identical — same source
   (`[[rcx+0x30]+0x20]`), same Z-flip, same frustum — but it is reached only via the single call at
   `0x1402507B9`, near the end of `RenderView`, and is used for an auxiliary pass (it also sets
   `GL_LIGHT0`, fog, and the billboard basis). Treat it as the **second** view-matrix site.
   **CONFIRMED that the matrix is identical in origin; INFERRED that its role is auxiliary.**

Ruled out, with the reason:

| site(s) | why it is not the scene view |
|---|---|
| `0x232524`, `0x238D8F` | `glMatrixMode(GL_TEXTURE)` — projected-texture matrices |
| `0x24B473`, `0x262DD6` | small helpers that just load whatever matrix the caller passes; used for sky/backdrop |
| `0x24EAF9/0x24EC3E/0x24ECA5` | `DrawObject` (root `0x14024E740`) restoring the *saved* view pointer between object draws — same value as the scene view, but not where it originates |
| `0x24EC77` | `transpose(object + 0x140)` — a per-object precomputed modelview |
| `0x227CB6` | `transpose(view × translate(camPos))` — the sky dome, anchored at the camera |
| `0x240A83`, `0x246180`, `0x246FDF`, `0x25BFCB`, `0x25E2A7`, `0x264A0C`, `0x255A1A` | `Mat4Mul(dst, ·, obj+0x20)` — per-object model-view |
| `0x261307` | particle/billboard matrix built at `r15+0x288` |
| `0x25082E` | `Mat4Mul(dst, view, listElement)` then a `glBegin(GL_LINES)` 24-vertex box — debug AABB |
| `0x24F4D6` | re-load of the already-built view matrix (`[rsp+0xD0]`) after `DrawObject` returns |

### 2.3 The `diag(1,1,-1,1)` Z-flip — CONFIRMED

Built on the stack at `0x14024FD87..0x14024FDF8` (and `0x14024EFC7..0x14024F051` in the other
variant): `m[0]=1.0, m[5]=1.0, m[15]=1.0, m[10]=xmm6`, everything else `0`.
`xmm6` is loaded at `0x14024F8ED` from `.rdata` VA `0x140353A18` = **`-1.0f`**
(the `1.0f` constant is at VA `0x140353808`).

Meaning: the engine's view matrix is **+Z-forward**; GL's −Z convention is applied on the
projection side. Anyone synthesising a replacement view matrix must build it +Z-forward.

### 2.4 Frame call chain (INFERRED from the call graph, CONFIRMED at each edge)

```
0x140135CD0 -> 0x14025A7D0  (Present)  -> 0x140254E40 -> ... -> SwapBuffers @0x14025A8B6
0x140120CC0 (world render) -> 0x140252BB0 -> {0x1402533C6, 0x1402545C0} -> RenderView 0x14024F740
0x14024F740 -> itself @0x140250312               (mirror / RTT sub-view, flags 0x1B)
0x140251E38 -> RenderView x6                     (cube-map faces / reflection probe)
0x1402517FF -> Camera::Update x6                 (same, 6 faces)
```
RenderView has **12 call sites** (`0x140250312, 0x1402512E6, 0x14025176B, 0x140252001, 0x1402520F0,
0x1402521DD, 0x1402522CC, 0x1402523BA, 0x1402524A9, 0x14025264E, 0x140254C45, 0x140254D3F`).
So a single frame issues **many** `frustum + Z-flip + view-load` sequences.

---

## 3. Backward trace: where the 16 floats come from

```
glLoadMatrixf(&stack[0xD0])                      @ VA 0x14024FF3B / RVA 0x24FF3B
        ^
Mat4Transpose(&stack[0xD0], viewState + 0x20)    @ VA 0x14024FF23 / RVA 0x24FF23
        ^
viewState = *(void**)(viewDesc + 0x30)           @ VA 0x14024FF13 / RVA 0x24FF13
        ^
Mat4InverseRigid(viewState + 0x20, viewState + 0x60)
                                                 @ VA 0x140249555 / RVA 0x249555
        ^
viewState + 0x60  =  CAMERA WORLD TRANSFORM (4x4 row-major, rigid)
```

### 3.1 Matrix-build function — **`Camera::Update` @ VA `0x140249510` / RVA `0x249510`** — CONFIRMED

```
0x140249510  mov  rax, rsp
0x140249513  push r13
0x140249515  sub  rsp, 0xA0
0x14024951C  cmp  dword [rcx+0x10], 0        ; disabled?
0x140249520  mov  r13, rcx                   ; r13 = viewDesc
0x140249523  jne  0x140249DFB
0x14024952D  mov  rbx, [rcx+0x30]            ; rbx = viewState
0x14024953D  lea  rsi, [rbx+0x60]            ; rsi = &viewState->worldMatrix
0x14024954E  lea  rcx, [rbx+0x20]            ; rcx = &viewState->viewMatrix
0x140249552  mov  rdx, rsi
0x140249555  call 0x140266750                ; view = InverseRigid(world)
0x14024955A  mov  r11d, [r13+0xC0]           ; projection type: 1 = explicit planes, 2 = fov, ...
             ... writes frustum planes into viewDesc+0x48/0x4C/0x50/0x54, +0x58..+0x64,
                 near +0x6C, far +0x70
```

Called from **22** sites; the important one is `0x14024F77F` — the **first thing `RenderView` does**.
So the view matrix is always regenerated from the world matrix at the start of every view render.
**CONFIRMED**

### 3.2 Struct layouts

**`viewState` — 0xE4 bytes, allocated from a static pool** (CONFIRMED):

| offset | type | meaning |
|---|---|---|
| `+0x00` | `int` | in-use flag (0 = free slot) |
| `+0x04` | `int` | — |
| `+0x08/0x0C/0x10` | `int` | zeroed on alloc |
| `+0x20` | `float[16]` | **VIEW matrix**, row-major = `InverseRigid(world)` |
| `+0x60` | `float[16]` | **CAMERA WORLD matrix**, row-major, rigid |
| `+0xA0` | `float[16]` | derived yaw-only / billboard basis (INFERRED — see below) |
| `+0xE0` | `int` | — |

Camera **position** = `viewState + 0x6C`, `+0x7C`, `+0x8C` (the `m03/m13/m23` column of the world
matrix). Camera **orientation** = the 3×3 at `+0x60,+0x64,+0x68 / +0x70,+0x74,+0x78 / +0x80,+0x84,+0x88`.
**It is a 3×3 orthonormal basis. Not euler angles. Not a quaternion.** **CONFIRMED**

**Static pool: VA `0x14057C0A0` .. `0x14057CB50` (RVA `0x57C0A0` .. `0x57CB50`), 12 × 0xE4.**
Read straight out of the allocator `0x140255C20`:
`lea rbp,[rip+0x326464]` @ `0x140255C35` → `0x14057C0A0`;
`lea rcx,[rip+0x326F01]` @ `0x140255C48` → `0x14057CB50`.
Constructor at `0x140255C69` does `Mat4Identity` on `+0x20`, `+0x60` and `+0xA0`. **CONFIRMED**

**`viewDesc`** (a **stack** object in the render path, e.g. `rsp + i*0x848 + 0x238` at
`0x140253981` — so there is *no* stable pointer to it): **CONFIRMED**

| offset | meaning |
|---|---|
| `+0x10` | int — non-zero = skip this view |
| `+0x14 / +0x18` | viewport width / height, as a fraction of the render target |
| `+0x1C / +0x24` | viewport x / y, as a fraction of the render target |
| `+0x30` | `viewState*` |
| `+0x38..+0x44` | fov / aspect inputs (used when `+0xC0 == 2`) |
| `+0x48/+0x4C/+0x50/+0x54` | frustum left / top / right / bottom |
| `+0x68` | aspect divisor |
| `+0x6C / +0x70` | zNear / zFar |
| `+0xC0` | projection type (1 = explicit planes, 2 = fov-based, …) |
| `+0xC4` | non-zero ⇒ use `glOrtho` instead of `glFrustum` |

Render-target size: `mov r11,[rip+…]` at `0x14024FE2D` → global pointer at
**VA `0x140584540` / RVA `0x584540`**; `*(int*)(*p + 0x18)` = width, `*(int*)(*p + 0x1C)` = height.
**CONFIRMED**

### 3.3 Derived camera direction written back into `viewState`

`SetupSceneCamera` (`0x14024EF80`) reads `viewState+0x40` (`m20`) and `viewState+0x48` (`m22`) —
the third row of the **view** matrix, i.e. the camera's world-space Z axis — normalises the
`(x, z)` part (falling back to row 1 when the camera looks near-vertically), and writes two
orthogonal horizontal vectors into `viewState + 0xA0/0xA4/0xA8`, `+0xB0/0xB4/0xB8` and
`+0xC0/0xC4/0xC8` (`0x14024F3B1..0x14024F42B`). This is a **yaw-only billboard basis** for
camera-facing sprites (trees, particles, flares). **INFERRED** (the read/write is CONFIRMED, the
purpose is deduced). Note that the world is **Y-up** — the code treats X/Z as the horizontal plane.
**CONFIRMED**

### 3.4 Who writes `viewState + 0x60`

* Constructor `0x140255C96` → identity. **CONFIRMED**
* `Mat4Copy(viewState+0x60, &stack[0xB0])` at **VA `0x140251005` / RVA `0x251005`**, inside root
  `0x140250C90` — a "build a view from a camera definition" routine (camera definitions are
  `0x1D8`-byte records, FOV read from `+0x3E8` of a parent block). Immediately after it,
  `0x140251028` does `mov [rdi+0x30], rbx`, wiring the `viewState` into a `viewDesc`. **CONFIRMED**
* Whether the *player/chase* camera goes through this same routine or writes `+0x60` from a
  different path is **UNKNOWN** — I did not find a second literal writer. The safest statement for
  cross-checking with the input-side agent: **whatever the input side produces, it must land in
  `*(viewState+0x60)` as a row-major rigid 4×4 before `Camera::Update` (`0x140249510`) runs, and
  `Camera::Update` runs at `0x14024F77F` at the top of every `RenderView`.**

---

## 4. Verdict on the API-boundary override

**Yes — a `glLoadMatrixf` hook can reliably identify the main scene view matrix at runtime, with
no game offsets at all.** Three properties make it easy, and all three are CONFIRMED from the
import table / disassembly:

1. No `glPushMatrix`/`glPopMatrix` ⇒ tracking "current matrix mode" is one variable.
2. `glMultMatrixf` is *only* ever applied to `GL_PROJECTION` ⇒ the modelview is only ever
   `glLoadIdentity` or `glLoadMatrixf`, so a hook sees the complete modelview state.
3. Every 3D scene pass emits the exact 3-call sequence
   `glMultMatrixf(diag(1,1,-1,1))` → `glMatrixMode(GL_MODELVIEW)` → `glLoadMatrixf(view)`
   with **no other GL call in between**, and the Z-flip matrix is a hard-coded constant.

### 4.1 Concrete discriminator recipe

Hook `glMatrixMode`, `glLoadIdentity`, `glLoadMatrixf`, `glMultMatrixf`, `glFrustum`, `glOrtho`,
`glViewport` and `SwapBuffers` (all plain IAT imports — no `wglGetProcAddress` needed).

```c
// per-thread state
GLenum  mode          = GL_MODELVIEW;
int     lastGLCall    = 0;          // token of the previous hooked GL call
int     sawFrustum    = 0;          // set by glFrustum, cleared by glMatrixMode(GL_PROJECTION)
int     sawZFlip      = 0;          // set by glMultMatrixf(diag(1,1,-1,1)) while mode==PROJECTION
GLint   vp[4];                      // last glViewport
GLint   winW, winH;                 // from the real window, e.g. cached at wglMakeCurrent
double  fr[6];                      // last glFrustum params

static int isZFlip(const GLfloat*m){                 // diag(1,1,-1,1), exact bit patterns
    return m[0]==1&&m[5]==1&&m[15]==1&&m[10]==-1&&
           m[1]==0&&m[2]==0&&m[3]==0&&m[4]==0&&m[6]==0&&m[7]==0&&
           m[8]==0&&m[9]==0&&m[11]==0&&m[12]==0&&m[13]==0&&m[14]==0;
}
static int isRigid(const GLfloat*m){                 // orthonormal 3x3 + affine bottom row
    if (m[3]||m[7]||m[11]) return 0;
    if (fabsf(m[15]-1.f) > 1e-4f) return 0;
    float l0=m[0]*m[0]+m[1]*m[1]+m[2]*m[2];
    float l1=m[4]*m[4]+m[5]*m[5]+m[6]*m[6];
    float l2=m[8]*m[8]+m[9]*m[9]+m[10]*m[10];
    return fabsf(l0-1)<1e-3f && fabsf(l1-1)<1e-3f && fabsf(l2-1)<1e-3f;
}

// hook_glMultMatrixf:
    if (mode==GL_PROJECTION && isZFlip(m)) sawZFlip = 1;
    lastGLCall = CALL_MULTMATRIX;

// hook_glMatrixMode:
    if (m==GL_PROJECTION) { sawFrustum = 0; sawZFlip = 0; }
    mode = m;  lastGLCall = CALL_MATRIXMODE_(m);

// hook_glFrustum: sawFrustum = 1; remember params; lastGLCall = CALL_FRUSTUM;
// hook_glOrtho:   sawFrustum = 0;                    lastGLCall = CALL_ORTHO;
// hook_glViewport: remember vp;                      lastGLCall = CALL_VIEWPORT;

// hook_glLoadMatrixf(m):
    int isSceneView =
           mode == GL_MODELVIEW
        && sawFrustum && sawZFlip
        && prevPrevCall == CALL_MULTMATRIX          // the Z-flip
        && lastGLCall   == CALL_MATRIXMODE_MODELVIEW
        && isRigid(m)
        && vp[2] >= winW*0.9 && vp[3] >= winH*0.9;  // full-window pass only
    if (isSceneView) m = my_replacement_view;       // +Z-forward!
    real_glLoadMatrixf(m);
    sawZFlip = 0;                                   // consume: one view load per projection epoch
    lastGLCall = CALL_LOADMATRIX;

// hook_SwapBuffers: reset everything, frame counter++
```

The `prevPrevCall == MULTMATRIX && lastGLCall == MATRIXMODE(MODELVIEW)` pair is the tight
signature that separates the *view* load from every other modelview load, and it also rejects the
sky/backdrop pass (whose next GL call after the Z-flip is `glCullFace`, VA `0x14024FB00`).

The replacement matrix must be **+Z-forward, column-major with translation in `m[12..14]`**, and
rigid (the game's own projection already contains the `diag(1,1,-1,1)`).

### 4.2 Failure modes and how each is handled

| pass | what the hook sees | handling |
|---|---|---|
| **Mirror / RTT sub-view** — `RenderView` recursing into itself at `0x140250312` with `flags=0x1B` | a *complete* second `frustum+Zflip+view-load` sequence inside the same frame | rejected by the full-window viewport test; if the mirror ever renders full-window, fall back to "take the sequence with the largest viewport / the first one of the frame" |
| **Cube-map / reflection probe** — 6 `RenderView` calls from `0x140251E38`, 6 `Camera::Update` from `0x1402517FF` | 6 more sequences, small square viewports | same viewport test (they are FBO-sized, not window-sized) |
| **Sky / backdrop** — `RenderView` pass 1 (`0x14024FA4B..0x14024FAE4`) | `frustum + Zflip` but the next GL call is `glCullFace`; the actual modelviews come from `0x14024B473` / `0x140262DD6` / `0x140227CB6` | rejected by the `prevPrev==MULTMATRIX` adjacency test |
| **Per-object modelviews** — 14 sites (`0x240A83`, `0x246180`, `0x246FDF`, `0x24EAF9`, `0x24EC3E`, `0x24EC77`, `0x24ECA5`, `0x255A1A`, `0x25BFCB`, `0x25E2A7`, `0x261307`, `0x264A0C`, `0x25082E`, `0x24B473`) | `GL_MODELVIEW` + rigid matrix, but with draw calls / texture binds in between | rejected by the adjacency test and by "consume one per projection epoch" |
| **HUD / 2D** — root `0x140245CB0` (`glOrtho` + `glMultMatrixf`), plus `0x140209BC0`, `0x140209D40`, `0x14022FFB0`, `0x14023CA30`, `0x14023D4A0/0x23D730/0x23D9E0/0x23DC40/0x23DF40`, `0x140264AF0` | `glOrtho` or identity/identity, never `glFrustum` | rejected by `sawFrustum` |
| **PiBoSo plugin `Draw()` overlay** | the plugin's quads are drawn by the 2D quad drawer at root `0x14023CA30` (`glBegin(GL_QUADS)` + `glColor4ubv` + `glTexCoord2fv` after `PROJ identity / MODELVIEW identity`) — **INFERRED**, but it is definitely a 2D identity/identity path | rejected by `sawFrustum` |
| **Post-process / bloom (GLSL, FBO)** — roots `0x140239290`, `0x140239DB0` | `glViewport` + PROJ identity + MODELVIEW identity + GLSL program | rejected by `sawFrustum` |
| **Shadows** | no dedicated depth-only `glFrustum` pass exists; shadow/projected textures go through `GL_TEXTURE` matrices (`0x140232524`, `0x140238D8F`) and the `TexCoordProj` `mat4` uniform | never seen as a MODELVIEW load |
| **Screenshot path** — root `0x140264AF0` (`glReadPixels`) | identity matrices | rejected by `sawFrustum` |

### 4.3 What the API-boundary override CANNOT fix (important)

Overriding at `glLoadMatrixf` changes only what the GPU is told. The game still:

* **culls and LODs against the original frustum** — geometry the real camera cannot see is never
  submitted, so a free camera will show holes/pop-in;
* **orients billboards from the original camera** (`viewState + 0xA0`, built in `0x14024EF80`) —
  trees/particles/flares will face the wrong way;
* **positions the sky dome at the original camera** (`0x140227CB6`).

The clean fix is the offset-based tier: write the camera world transform at
`viewState + 0x60` **before** `Camera::Update` (`0x140249510`) runs, e.g. by hooking
`0x140249510` and rewriting `*(float(*)[16])(*(uintptr_t*)(rcx+0x30) + 0x60)`. That propagates to
the view matrix, the frustum planes, the billboard basis and the sky in one shot.
**Recommendation: ship the API hook as the patch-proof fallback, and the `+0x60` write as the
preferred path when the offsets resolve.**

---

## 5. Shader path — does the fixed-function hook cover everything?

**Yes.** **CONFIRMED**

* No shader entry points are *imported*; they are all resolved through `wglGetProcAddress`
  (62 call sites) in the resolver `0x140229CD0`. Resolved slots (RVA of the global function
  pointer, followed by number of call sites):

  | proc | slot VA | slot RVA | calls |
  |---|---|---|---|
  | `glCreateProgramObjectARB` | — | — | 1 |
  | `glCreateShaderObjectARB`  | `0x140398FB8` | `0x398FB8` | 1 |
  | `glShaderSourceARB`        | `0x140398FC0` | `0x398FC0` | 2 |
  | `glCompileShaderARB`       | `0x140398FC8` | `0x398FC8` | 2 |
  | `glAttachObjectARB`        | `0x140398FD8` | `0x398FD8` | 3 |
  | `glLinkProgramARB`         | `0x140398FE0` | `0x398FE0` | 2 |
  | `glUseProgramObjectARB`    | `0x140398FE8` | `0x398FE8` | 1 (single wrapper) |
  | `glGetUniformLocationARB`  | `0x140398FF0` | `0x398FF0` | 17 |
  | `glGetAttribLocationARB`   | `0x140398FF8` | `0x398FF8` | 402 |
  | `glUniform1iARB`           | `0x140399000` | `0x399000` | 23 |
  | `glUniform1fARB`           | `0x140399008` | `0x399008` | 171 |
  | `glUniform2fvARB`          | `0x140399010` | `0x399010` | 71 |
  | `glUniform3fARB`           | `0x140399018` | `0x399018` | 26 |
  | `glUniformMatrix3fvARB`    | `0x140399020` | `0x399020` | 42 |
  | `glUniformMatrix4fvARB`    | `0x140399028` | `0x399028` | **3** (`0x140234CDF`, `0x14023505B`, `0x1402352BB`) |
  | `glVertexAttribPointerARB` | `0x140399040` | `0x399040` | 18 |

* FBOs are used (`GL_EXT_framebuffer_object`): `glBindFramebufferEXT` slot `0x140398F48` /
  RVA `0x398F48`, 4 call sites (`0x14022814E`, `0x1402281BD`, `0x14023A280`, `0x14023A4CD`);
  `glBlitFramebufferEXT` slot `0x140398F90` / RVA `0x398F90`, 5 sites. Multisample resolve and
  post-processing. **CONFIRMED**

* **The GLSL sources are embedded in `.data` from ~RVA `0x37C520` onward** (87 `#version` strings).
  Verbatim from RVA `0x37C710`:

  ```glsl
  #version 110
  varying vec3 LightDir; varying vec3 EyeDir; uniform vec3 LightPosition; attribute vec4 Tangent;
  void main(){
    vec3 vertexPosition = vec3(gl_ModelViewMatrix * gl_Vertex);
    gl_Position    = ftransform();
    gl_TexCoord[0] = gl_MultiTexCoord0;
    vec3 n = normalize(gl_NormalMatrix * gl_Normal);
    ...
    gl_FogFragCoord = gl_Position.z;
  }
  ```

  String counts: `ftransform` ×29, `gl_ModelViewMatrix` ×15, `gl_NormalMatrix` ×36,
  `gl_Vertex` ×36, `gl_Position` ×56. **Zero** occurrences of an explicit
  `uniform mat4 modelView`-style transform; the only `mat4` uniform is `TexCoordProj`
  (the projected/shadow texture matrix — that is what the 3 `glUniformMatrix4fvARB` calls upload).

  ⇒ **Every shader-drawn object still transforms through the fixed-function `GL_MODELVIEW` /
  `GL_PROJECTION` matrices.** A `glLoadMatrixf` hook covers the entire scene. **CONFIRMED**

* There is a config key `shaders_disable` (string at RVA `0x340178`, referenced from
  `0x1400D7481` and `0x1400ED53A`) — shaders can be turned off entirely, which only makes the
  fixed-function path *more* complete. **CONFIRMED**

---

## 6. AOB signatures

`??` = volatile (RIP-relative disp32 or rel32). All are 32 bytes.

**A. MAIN SCENE VIEW load — `viewState` deref + transpose + `glMatrixMode` + `glLoadMatrixf`**
anchor VA `0x14024FF13` / RVA `0x24FF13` (the `glLoadMatrixf` is at anchor+0x28):
```
48 8B 55 30 48 8D 8C 24 D0 00 00 00 48 83 C2 20 E8 ?? ?? ?? ?? B9 00 17 00 00 FF 15 ?? ?? ?? ??
```
raw: `48 8B 55 30 48 8D 8C 24 D0 00 00 00 48 83 C2 20 E8 A8 67 01 00 B9 00 17 00 00 FF 15 65 18 0D 00`
*(`48 8B 55 30` = `mov rdx,[rbp+0x30]`, `48 83 C2 20` = `add rdx,0x20`, `B9 00 17 00 00` = `mov ecx,GL_MODELVIEW`.
The `48 8D 8C 24 D0 00 00 00` stack displacement is compiler-chosen — mask bytes 8..11 if you want
it version-tolerant.)*

**B. The Z-flip + frustum + MODELVIEW selection that opens the main pass**
anchor VA `0x14024FE1C` / RVA `0x24FE1C`:
```
B9 01 17 00 00 FF 15 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 4C 8B 1D ?? ?? ?? ?? 49 8B 03 66 0F 6E 50 1C
```
raw: `B9 01 17 00 00 FF 15 71 19 0D 00 FF 15 9B 19 0D 00 4C 8B 1D 0C 47 33 00 49 8B 03 66 0F 6E 50 1C`
*(`glMatrixMode(GL_PROJECTION=0x1701)`, `glLoadIdentity`, then the render-target-size global.)*

**C. `Camera::Update` — `view = InverseRigid(world)`**
anchor VA `0x14024954E` / RVA `0x24954E`:
```
48 8D 4B 20 48 8B D6 E8 ?? ?? ?? ?? 45 8B 9D C0 00 00 00 41 83 FB 01 0F 85 ?? ?? ?? ?? F3 41 0F
```
raw: `48 8D 4B 20 48 8B D6 E8 F6 D1 01 00 45 8B 9D C0 00 00 00 41 83 FB 01 0F 85 45 01 00 00 F3 41 0F`
*(`lea rcx,[rbx+0x20]` = &view, `mov rdx,rsi` = &world, then `call Mat4InverseRigid`.
Scan a bit earlier — VA `0x140249541` — if you want the whole prologue.)*

**D. `viewState` static pool bounds (gives you `0x14057C0A0` and `0x14057CB50`)**
anchor VA `0x140255C35` / RVA `0x255C35`:
```
48 8D 2D ?? ?? ?? ?? 45 33 E4 48 8B F1 48 8B C5 41 8B FC 48 8D 0D ?? ?? ?? ?? 90 44 39 20 74 14
```
raw: `48 8D 2D 64 64 32 00 45 33 E4 48 8B F1 48 8B C5 41 8B FC 48 8D 0D 01 6F 32 00 90 44 39 20 74 14`
*(resolve both RIP-relative displacements to recover pool base and pool end.)*

**E. Second view load (`SetupSceneCamera`, `0x14024EF80`) — transpose+copy+`glLoadMatrixf`**
anchor VA `0x14024F1B4` / RVA `0x24F1B4` (the `glLoadMatrixf` is at `0x14024F1F1`):
```
48 8D 94 24 10 01 00 00 48 8D 8C 24 50 01 00 00 E8 ?? ?? ?? ?? 48 8D 94 24 50 01 00 00 48 8D 8C
```
raw: `48 8D 94 24 10 01 00 00 48 8D 8C 24 50 01 00 00 E8 07 75 01 00 48 8D 94 24 50 01 00 00 48 8D 8C`

**F. Debug-AABB modelview (`view × obj`) — useful as a negative control**
anchor VA `0x14025080C` / RVA `0x25080C`:
```
48 8D 94 24 D0 00 00 00 48 8D 4C 24 50 E8 ?? ?? ?? ?? B9 00 17 00 00 FF 15 ?? ?? ?? ?? 48 8D 4C
```

**G. Sky-dome modelview (`view × translate(camPos)`) — negative control**
anchor VA `0x140227CA1` / RVA `0x227CA1`:
```
E8 ?? ?? ?? ?? B9 00 17 00 00 FF 15 ?? ?? ?? ?? 48 8D 4C 24 60 FF 15 ?? ?? ?? ?? 4C 8D 45 6C 49
```

---

## 7. Address quick-reference (for cross-checking with the input-side agent)

| what | VA | RVA | label |
|---|---|---|---|
| **MAIN scene `glLoadMatrixf`** | `0x14024FF3B` | `0x24FF3B` | CONFIRMED |
| `glMatrixMode(GL_MODELVIEW)` before it | `0x14024FF2D` | `0x24FF2D` | CONFIRMED |
| `Mat4Transpose(&tmp, viewState+0x20)` | `0x14024FF23` | `0x24FF23` | CONFIRMED |
| `mov rdx,[viewDesc+0x30]` (viewState deref) | `0x14024FF13` | `0x24FF13` | CONFIRMED |
| Secondary scene `glLoadMatrixf` | `0x14024F1F1` | `0x24F1F1` | CONFIRMED |
| `RenderView` (root) | `0x14024F740` | `0x24F740` | CONFIRMED |
| `SetupSceneCamera` (root) | `0x14024EF80` | `0x24EF80` | CONFIRMED |
| `DrawObject` (root) | `0x14024E740` | `0x24E740` | CONFIRMED |
| **`Camera::Update(viewDesc)`** | `0x140249510` | `0x249510` | CONFIRMED |
| `view = InverseRigid(world)` call | `0x140249555` | `0x249555` | CONFIRMED |
| `Mat4Identity` | `0x140266390` | `0x266390` | CONFIRMED |
| `Mat4Mul` | `0x1402663E0` | `0x2663E0` | CONFIRMED |
| `Mat4Copy` | `0x1402665F0` | `0x2665F0` | CONFIRMED |
| `Mat4Transpose` | `0x1402666D0` | `0x2666D0` | CONFIRMED |
| `Mat4InverseRigid` | `0x140266750` | `0x266750` | CONFIRMED |
| **`viewState` pool base** | `0x14057C0A0` | `0x57C0A0` | CONFIRMED |
| `viewState` pool end | `0x14057CB50` | `0x57CB50` | CONFIRMED |
| `viewState` allocator | `0x140255C20` | `0x255C20` | CONFIRMED |
| `viewState` slot ctor | `0x140255C69` | `0x255C69` | CONFIRMED |
| writer of `viewState+0x60` (one path) | `0x140251005` | `0x251005` | CONFIRMED |
| render-target size global (ptr→ptr) | `0x140584540` | `0x584540` | CONFIRMED |
| `-1.0f` (Z-flip constant) | `0x140353A18` | `0x353A18` | CONFIRMED |
| `1.0f` | `0x140353808` | `0x353808` | CONFIRMED |
| `SwapBuffers` call site | `0x14025A8B6` | `0x25A8B6` | CONFIRMED |
| Present / end-of-frame (root) | `0x14025A7D0` | `0x25A7D0` | CONFIRMED |
| World-render entry (root) | `0x140120CC0` | `0x120CC0` | INFERRED |
| GLSL proc resolver | `0x140229CD0` | `0x229CD0` | CONFIRMED |
