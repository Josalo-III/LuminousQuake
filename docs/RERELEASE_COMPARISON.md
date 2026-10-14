# Rerelease QuakeC comparison — October 14, 2026

## Conclusion

Enhancing the original game remains the primary goal. Treat rerelease game data
as an expected installation that the engine must handle resiliently, rather than
as a replacement foundation or a commitment to reproduce the entire rerelease.
The supplied installed
`progs.dat` already needs more engine services than LuminousQuake provides.
The latest published QuakeC is a third, distinct target: it is not proven to be
the source of that installed binary, and replacing the binary with a new build
would introduce additional requirements and gameplay changes.

For supported rerelease content, implement the required engine services with
defined behavior and regression coverage. Detect unsupported program requirements
at load time where possible and explain what is unsupported and how to select
compatible data; a late combat/ending crash is not acceptable handling of a known
incompatibility. Static checks cannot establish every runtime requirement, so
capability checks must be paired with campaign testing before claiming support.
Keep original-game behavior as the reference, preserve installed data, and expose
only extensions that the engine actually implements. Additional campaigns, bots
and rerelease-specific modes remain separate scope decisions.

Rerelease visual upgrades are explicitly within scope as candidates for improving
the original campaign. Evaluate them alongside ioquake3 and Doom 3 techniques;
they need not be adopted as a package with rerelease gameplay or online services.
Engine support for visual features belongs here, while creation of enhanced
game data remains a separate project. Preserve the Classic appearance option.

Local content compatibility does not establish rerelease multiplayer
interoperability. Preserve classic multiplayer behavior, but investigate the
rerelease's protocol, gameplay expectations and service requirements separately
before promising access to its servers. This comparison does not establish those
requirements or server compatibility.

The primary immediate gaps are localization with arguments, gib movement and
the installed program's builtins 79/80. This checkpoint includes fixes for the
first two and generated regression coverage. Builtins 79/80
remain missing. Prior successful smoke tests did not establish full-campaign
compatibility and did not exercise those calls.

## Sources and method

- Published rerelease source: [id-Software/quake-rerelease-qc](https://github.com/id-Software/quake-rerelease-qc/tree/634eefab09a77eb7b5f5ca7078ba3d8784a91142),
  revision `634eefab09a77eb7b5f5ca7078ba3d8784a91142`.
  Downloaded to `/private/tmp/lq-quake-rerelease-qc`.
- Original single-player QuakeC reference: [id-Software/Quake-Tools, qcc/v101qc](https://github.com/id-Software/Quake-Tools/tree/c0d1b91c74eb654365ac7755bc837e497caaca73/qcc/v101qc),
  revision `c0d1b91c74eb654365ac7755bc837e497caaca73`.
  Downloaded to `/private/tmp/lq-quake-tools`. This is an original 1.01 source
  reference, not an authenticated copy of the user's 1996 disc or a claim that
  it matches every original 1.09 behavior. The QuakeWorld QC in our engine
  checkout is not the single-player comparison baseline.
- Engine: LuminousQuake `e41d057`, with this checkpoint's subsequent print/gib
  fixes inspected explicitly. Engine builtin table and VM execution/load paths
  were inspected, not inferred from file names.
- Installed archive: read `progs.dat` directly from the ignored local
  `quake-complete/Quake/id1/pak0.pak`; no extraction or modification of game data.
- Manifest-based lexical inventory of all six `progs.src` source sets, builtin
  declarations, call sites, constants, extension queries and cvar reads. Checked
  original engine-owned declarations after removing comments/whitespace.
- This did **not** compile FTEQCC, rebuild/replace `progs.dat`, preprocess every
  conditional configuration, establish runtime reachability of all call sites,
  or validate expansion assets/campaigns. Declarations alone are not requirements.
  Original expansion sources were not obtained, so differences between those
  campaigns and the original base campaign are not attributed solely to the
  rerelease.

The repository describes a 2021 rerelease lineage, but the pinned source also
includes later work, including Dawn of the Machine and 2026 changes. Calling
this exact checkout simply “the 2021 build” would be misleading.

## Original engine interface and installed binary

All six published `defs.qc` files retain the original engine-owned global and
entity field declarations in the same token order and types. Additional QC-owned
fields after those markers do not by themselves require changing `entvars_t`.

The installed program has:

| Property | Observed value |
| --- | --- |
| QuakeC format | v6 |
| System-layout CRC | 5927, accepted by this engine |
| Entity fields | 199 |
| Functions / statements | 2,099 / 20,952 |
| Observed opcode range | All observed opcodes are within the engine's original 0–65 range |
| Missing numeric builtins | `finaleFinished` → 79; `localsound` → 80 |
| Program SHA-256 | `dad5186b78b66000b3e53fbaf255d0197d39270b9fcd96fcdbdaaa9e5aa563c7` |

The engine has 79 builtin-table entries: slots **0–78**, including unimplemented
slots. A nonnegative slot count is not evidence that calls 79/80 work. The VM
currently raises `Bad builtin call number` if either is executed. These are
latent failures even though ordinary movement/combat smoke tests passed.

The installed program does not declare a `gravity` entity field. Its gib fix
therefore uses the existing default-gravity path. The engine already supports
an optional QC `gravity` field through `SV_AddGravity`; that field exists in
the published Rogue codebase. It must not be confused with a mandatory change
to the fixed engine struct.

Modern source syntax and an FTEQCC recommendation do not establish a need for a
different runtime VM: modern compilers can lower language features to original
bytecode. The installed program demonstrates that distinction. The output of a
fresh compilation of the latest sources remains untested.

## Engine service comparison

| Area | Evidence | LuminousQuake state and consequence |
| --- | --- | --- |
| Localization | `defs.qc` print declarations; `$qc_item_health` formats with `{0}` in the installed English table | Map/ending exact-key lookup and parameter-aware server `bprint`/`sprint`/`centerprint` are implemented. Localization must happen before QC argument concatenation loses boundaries. |
| Gib movement | Base `player.qc` uses type 11, gated by `EX_MOVETYPE_GIB` in the latest source | The reported installed binary produced type 11 without a usable engine guard. Working-tree support routes it through bounce physics with gravity. It is **not** the gravity-free `QUAKE2` type 11 in the original source. |
| Finale completion | Installed builtin 79; published `monsters/shub.qc` calls `finaleFinished()` | Missing. Define/test local ending/credits semantics; do not silently return success merely to avoid a crash. |
| Local sound | Installed builtin 80; published `ex_localsound` declaration | Missing. Needs sound delivered to the designated client without globally broadcasting it. Declaration in latest source does not prove a base-source call site; the installed binary requirement is separately established. |
| Named builtin binding | Published `#0:ex_*` declarations | Loader/VM currently dispatch numeric negative statement entries only. No named binding layer is implemented. A freshly compiled modern program cannot be assumed load-compatible without checking its compiler output. |
| Extension negotiation | `checkextension` 99 and `pr_checkextension`; optional `EX_MOVETYPE_GIB` / `EX_EXTENDED_EF` checks | Neither query service nor capability cvar is registered. Some current-source paths fall back when the cvar is zero. Only advertise extensions whose behavior is implemented and tested. |
| Weapon switching preference | Base `weapons.qc:W_WantsToChangeWeapon` calls `CheckPlayerEXFlags` | Missing named service. A zero-flags legacy preference policy is plausible, but requires an intentional implementation and pickup tests. |
| AI/pathfinding and bots | `ai.qc:ai_pathtogoal`; bot source sets; named bot/path functions | Missing modern path services. The source has ordinary `movetogoal` fallbacks in some paths, but a missing function must still be bound safely before those paths can work. Bot support is a separate scope decision. |
| Extended effect bits | Quad/pentagram/candle lights, values 16/32/64, have real source references | Client currently handles original brightfield/muzzle/bright/dim effects, not those additional light meanings. Geometry-only success does not verify them. |
| Corpse solidity | `SOLID_CORPSE = 5` declared in base `defs.qc` | No occurrence outside `defs.qc` in the manifest source sets inspected; not an immediate demonstrated requirement. Original collision handling has no explicit corpse semantics. Revisit if a compiled program/assets exercise it. |
| Protocol | Achievement 52 is actually emitted; other newer SVC constants are declared | Achievement 52 is consumed. No non-definition references were found for 39 or 53–57 in the manifest QC checked. Do not implement their wire layouts from numeric declarations alone. `TE_EXPLOSION2`, `TE_BEAM` and cutscene 34 already have older-engine support; being absent from the 1.01 QC definitions does not make them missing here. |
| Fog/alpha | MG3 `fog.qc` sends `fog` commands; `lights.qc` and `misc_fx.qc` set alpha | No matching fog command or entity-alpha submission path. These affect expansion appearance/behavior; they are not evidence that base gameplay needs Enhanced rendering. Traditional fog/alpha can belong to Classic GPU. |
| Campaign/cheat metadata | `campaign`, `sv_cheats` and additional cvar reads | `campaign` is registered. `sv_cheats` semantics are not the same as original command-side cheat checks and need review before claiming modern game-code parity. |

## Coverage across the six published codebases

Counts come from manifest-listed files only. Named builtin counts are
declarations; the “referenced” column lists syntactic call sites outside
`defs.qc`, not a proof those calls execute in every game mode.

| Source set | QC files | Named declarations | Additional numeric slots | Notable referenced named services beyond formatted printing |
| --- | ---: | ---: | --- | --- |
| Base `quakec` | 33 | 18 | 99 extension query | Finale completion, player flags, pathfinding |
| CTF `quakec_ctf` | 22 | 21 | 99; `setcolor` 401 | Player flags; prompt, prompt-choice and clear-prompt services |
| Scourge `quakec_hipnotic` | 55 | 18 | 99 | Finale completion, player flags |
| Dimension `quakec_mg1` | 45 | 18 | 99 | Player flags, pathfinding |
| Dawn `quakec_mg3` | 63 | 18 | 99 | Player flags, pathfinding, debug bounds drawing |
| Dissolution `quakec_rogue` | 61 | 18 | 99 | Finale completion, player flags |

Shared declared services also include debug drawing and bot helpers which are
not all referenced in all manifests. CTF's `setcolor` is extension-gated in the
source; its declaration alone is not evidence that ordinary base gameplay calls
slot 401. Dimension of the Past shares the Dimension codebase per id's README;
there is no separate seventh source set to audit here.

## What to do next

Startup diagnostics now distinguish an absent optional `autoexec.cfg` from a
failed required asset load. The installed `quake.rc` requests `scr_usekfont 1`;
this is reported as unsupported, with the original bitmap font retained. The
installed E1M1 worldspawn contains a `fog` value; when no QC field handles it,
the engine reports that fog is unsupported and renders without it. These
diagnostic changes do not implement rerelease fonts or fog rendering. Fog remains
an in-scope visual compatibility task.

1. Finish and verify the parameterized print and gib fixes against the installed
   program; retain literal-message behavior for original game code.
2. Implement/test numeric 79/80 for this identified binary, with explicit ending
   and targeted-sound semantics. Add endgame, gib-producing combat and pickup
   checks rather than relying solely on the existing short smoke sequence.
3. Add a load-time builtin capability audit so unsupported imports are reported
   before play, distinguishing optional named imports from required calls.
4. Establish named binding and conservative extension-query behavior; inspect a
   pinned FTEQCC build's output before supporting newly compiled rerelease QC.
5. Evaluate rerelease visual improvements, including fog/alpha and additional
   effects, for the original campaign and appropriate renderer modes, with
   focused tests. Expansion/bot/CTF support and rerelease server interoperability
   remain separate scope decisions; prompts, AI and metadata services require
   an explicitly selected content scope.

Do not overwrite the installed `progs.dat`, import rerelease gameplay changes
into the engine indiscriminately, or replace the original compatibility target.
Use separate original-game and rerelease-content test matrices. Current native
Mac validation is not Windows/Linux runtime validation.

## Reproduce the inventory

The source checkouts are temporary and can be recreated from the pinned public
revisions above. From the LuminousQuake repository:

```sh
python3 tools/audit_rerelease.py \
  --rerelease /private/tmp/lq-quake-rerelease-qc \
  --original /private/tmp/lq-quake-tools \
  --pak quake-complete/Quake/id1/pak0.pak \
  --output build/rerelease-audit.json
```

The JSON output stays in the ignored build directory. This report records only
metadata/findings, not PAK contents, strings tables, compiled game code or assets.
