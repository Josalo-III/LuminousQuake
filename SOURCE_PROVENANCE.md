# Source provenance

## Initial engine

- Source: https://github.com/id-Software/Quake
- Upstream revision: `bf4ac424ce754894ac8f1dae6a3981954bc9852d`
- Imported by cloning the complete repository; original history and file notices retained.
- License: original GPL release; source notices specify GPL-2.0-or-later. See `gnu.txt` and `readme.txt`.
- Build selection: portable NetQuake C code under `WinQuake/`; QuakeWorld remains separate.

## Phase 1 adaptations

New `platform/` adapters and diagnostic code, CMake/test infrastructure, and the source fixes described in `docs/PHASE_1.md` were developed for LuminousQuake. New C code and fixture code use GPL-2.0-or-later. No ioquake3, Doom 3, dhewm3, FTE, or Tenebrae implementation files have been copied into this phase.

Before a future donor import, record its repository URL, exact revision, original paths, file licenses, local modifications, and validation evidence here. Confirm license compatibility of the selected files and preserve their notices.
