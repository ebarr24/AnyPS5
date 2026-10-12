# Bounded resolver diagnostic inputs

This is a private, focused GitHub Actions diagnostic for AnyPS5's public native guest relinking and user-space PRX/libkernel boundary. It is not a production collector and does not create qualifying `ci.json` or Full-suite evidence.

The workflow compares three cases against pinned source: original base `9aa5fd290007d816bfed6970ee259f9baaa0e24e` for the existing cold cancellation base-red and three controls; previous candidate `b758d156dd850da84e166b14f282cbba393d331c` for the resolver's old unbounded waiter; and bounded candidate `0186b61026aefd76124ef30b9b07de8a43b3bb05` for four existing essential cases plus the new resolver contention test.

The previous-candidate lane receives only the identical public test/header/CMake fixture and `legacy-resolver-test-hooks.patch`. That patch adds compile-gated atomics to hold the winning first resolver and count a contender. It deliberately retains the original unbounded acquire loop and does not copy the production fix. The test releases the owner after a finite observation interval, then requires the prior implementation to fail its named `returnedBeforeOwnerRelease` assertion. The intended failure is separate from base 9aa's original SIGSEGV cancellation evidence.

The head contention case runs in a fresh CTest process, does not prewarm the cancellation process, checks a held winner and, only after an acquire-observed return, the contender's untouched output, then requires successful publication and reuse of the cached personality/Resume pair. Test hooks are under `APS5_HOST_UNWIND_TESTING`; production observer behavior remains null-default and unwind-path instrumentation has no callbacks, allocation, logging or locks.

The candidate run raises CTest's passed/failed output capture limits to 1 MiB after checking that the installed CTest supports both options, and uploads the raw `LastTest.log` as well as XML and the `--output-log` file. The workflow rejects a truncation marker and checks contiguous printed trace indices so exact event totals and owner names can be reviewed. It keeps the actor/triggering-actor write checks, fork-parent check, schema-only `3391` input, read-only permissions and bounded essential-test case accounting.

No source/base/head push or workflow dispatch is authorized by this package preparation. The source commit, driver commit, new validation refs and any future one-run dispatch require a separate coordinator release. The workflow does not invoke `ci.py`, change production collectors, run Full, publish a PR or make a title/gameplay claim.
