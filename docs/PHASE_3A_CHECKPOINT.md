# Phase 3A checkpoint — October 14, 2026

In progress: separate engine user data; executable/app installation discovery;
native Mac folder picker and remembered installation; Finder startup menu;
CMake app bundle with embedded SDL3, owned icon and local ad-hoc signing;
Options → Video Options resolution/fullscreen/vsync staging and Apply.

Earlier validation passed 16 tests and the isolated software gameplay smoke
confirmed saves/demos in the separate user directory with installation files
unchanged. The app was launched through Launch Services and its main menu was
visually observed. The installation preference points to the local test archive.

Latest validation: 15/16 tests pass. `luminousquake-app-fixtures` times out because
its missing-data case supplies no explicit `-basedir`; the new discovery path
finds the remembered real installation instead of the fixture's working directory.
Update fixture invocations to provide their intended base directory, preserving
explicit override cases. Then rerun the complete suite and sanitizer/content
checks. Video-menu staging/Apply checks pass in the SDL platform tests.

Unfinished: chooser cancellation and invalid-folder UI verification; independent
app-relative discovery tests; saved-resolution relaunch checks; final Release
packaging/install documentation; remaining long-path/write-path review; other
platform runtime validation. Public distribution still needs Developer ID signing
and notarization; the present bundle is a local development build.

The user stopped work due to usage limits. Do not treat this checkpoint as a
completed Phase 3A delivery. Game data and generated app artifacts remain ignored.
