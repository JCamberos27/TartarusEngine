# Editor conventions

Short, load-bearing rules that aren't obvious from the code. Add to this file rather than
letting a convention live only in a commit message or a comment nobody reads.

## Ship components and controls fully wired, or not visible (#215)

The 2026-09-04 audit found the same problem five times, in both directions: controls the editor
shows that do nothing, and runtime behaviour with no controls. Both waste the user's time — a
dead control invites you to set it, save it, and build on an assumption that was never true; an
undiscoverable feature may as well not exist.

1. **A component or import setting is not shown in the editor until its runtime behaviour is
   implemented.** If the field must exist for serialization / forward-compat, keep it in the
   struct and the serializer but leave it out of the Inspector.

2. **A component that ships with runtime behaviour ships with an Inspector section and an Add
   Component menu entry in the same change** — not a follow-up.

3. **Where something genuinely must be visible before it works** (a user would reasonably expect
   the control to exist), show it **disabled** with an explicit `(not implemented)` label and a
   tooltip pointing at the tracking issue — never rely on a code comment the user never sees.

The native component registration system (#184) is the structural fix: if declaring a component
generates its serialization, Inspector section and menu entry together, neither failure mode is
possible by construction. This convention is what to follow until that lands.

### Current applications

| Control | Treatment | Unblocks when |
|---|---|---|
| `ColliderComponent::IsTrigger` | shown disabled, `(not implemented)` — a collider is a place a user looks for a trigger toggle (rule 3) | #185 (collision system) |
