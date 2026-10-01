#include "imperivm/core/sim/hooks.hpp"

#include "imperivm/core/sim/world.hpp"

namespace imperivm::core::sim {

ClassHookShape class_hook_shape(ClassHook hook) noexcept {
  switch (hook) {
    case ClassHook::on_die:
      return ClassHookShape{"ondie", 1};
    case ClassHook::on_kill:
      return ClassHookShape{"onkill", 2};
    case ClassHook::on_enter:
      return ClassHookShape{"onenter", 2};
  }
  return ClassHookShape{{}, 0};
}

bool fire_class_hook(World& world, ClassHook hook, ObjectId subject, ObjectId argument) {
  // The first of the original's three guards: a null receiver returns without
  // touching anything, which here is a handle that no longer resolves.
  //
  // **Redundant as a refusal and load-bearing as a precondition**, which a
  // fault sweep established rather than left to judgment: removing the refusal
  // changes no outcome, because the runner resolves the object again and
  // answers false for one it cannot find. What the lookup is really for is the
  // latch below, which has nowhere to live without it. Kept in the original's
  // order, and annotated rather than deleted or tested.
  ObjectState* state = world.mutable_state(subject);
  if (state == nullptr) return false;

  // The second, and the only one that is per-object. `ondie` is the only hook
  // 0x005141b0 fires, so it is the only one the latch governs: `onkill` fires
  // once per kill and `onenter` once per entry, both of which can legitimately
  // happen to one object many times.
  if (hook == ClassHook::on_die) {
    if (state->flags.ondie_fired) return false;
    state->flags.ondie_fired = true;
  }

  // The third, `[0x9c0824]`. A world with no runner is a world with nothing to
  // run a script in, and the latch above is taken all the same.
  ClassHookRunner* runner = world.hook_runner();
  if (runner == nullptr) return false;
  return runner->run_class_hook(hook, subject, argument);
}

}  // namespace imperivm::core::sim
