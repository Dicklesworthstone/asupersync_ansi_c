# API Documentation Contract

Public API documentation policy for the ASX ANSI C runtime kernel.
Every `ASX_API` function must include structured documentation that allows
consumers to use the API safely without consulting internal source code.

## Mandatory Fields Per Public API

Each `ASX_API` function's header comment must include:

| Field | Required | Format |
|-------|----------|--------|
| **Brief description** | Yes | One sentence describing what the function does |
| **Preconditions** | If any | Constraints on arguments and state |
| **Postconditions** | If any | State changes on success |
| **Error codes** | If non-void | All possible return values |
| **Ownership** | If pointers | Who owns allocated memory; lifetime rules |
| **Thread-safety** | If relevant | Safe/unsafe, which locks required |

## Documentation Style

Plain C block comments. No Doxygen tags.

```c
/* Spawn a task within a region.
 * The poll_fn will be called by the scheduler until it returns ASX_OK
 * or an error.
 *
 * Preconditions: region must be OPEN, poll_fn must not be NULL.
 * Returns ASX_OK on success, ASX_E_REGION_CLOSED if closed,
 * ASX_E_REGION_POISONED if poisoned, ASX_E_RESOURCE_EXHAUSTED if arena full.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_task_spawn(asx_region_id region,
                                                asx_task_poll_fn poll_fn,
                                                void *user_data,
                                                asx_task_id *out_id);
```

## Error Documentation Requirements

1. Every `asx_status`-returning function must list all possible error codes.
2. Error behavior must match entries in `docs/API_MISUSE_CATALOG.md`.
3. Error codes must use the family taxonomy from `include/asx/asx_status.h`.

## Cross-References

- Error taxonomy: `include/asx/asx_status.h`
- Misuse catalog: `docs/API_MISUSE_CATALOG.md`
- Transition tables: `docs/LIFECYCLE_TRANSITION_TABLES.md`
- Safety profiles: `docs/SAFETY_PROFILES.md`

## Lint Enforcement

The `tools/ci/check_api_docs.sh` script validates:
- Every `ASX_API` function has a preceding comment block
- Comment block is non-trivial (more than just a blank line)
- Functions returning `asx_status` mention at least one error code or "Returns"

Run manually: `./tools/ci/check_api_docs.sh`

CI integration: the script exits with nonzero status on violations,
suitable for use as a gate in the CI workflow matrix.

## Reviewer Checklist

When reviewing a PR that adds or modifies public API:

- [ ] Function has a brief description (what, not how)
- [ ] Preconditions stated if arguments are constrained
- [ ] All possible error codes listed
- [ ] Ownership of pointer parameters documented
- [ ] Thread-safety noted (default: single-threaded)
- [ ] Misuse catalog updated if new error paths added
- [ ] Lint gate passes (`tools/ci/check_api_docs.sh`)

## Waiver Process

If a function genuinely has no preconditions, errors, or ownership
concerns (e.g., a pure query returning a scalar), it may omit those
fields. The lint script recognizes minimal-doc functions (void return,
no pointer params) and skips them.
