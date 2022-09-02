#ifndef INSTRUCTIONS_H
#define INSTRUCTIONS_H

#include "CPUState.h"
#include "SGPWorld.h"
#include "Tasks.h"
#include "sgpl/hardware/Cpu.hpp"
#include "sgpl/operations/flow_global/Anchor.hpp"
#include "sgpl/program/Program.hpp"
#include "sgpl/spec/Spec.hpp"
#include "sgpl/utility/ThreadLocalRandom.hpp"
#include <mutex>

namespace inst {

/**
 * Macro to easily create an instruction:
 * `INST(MyInstruction, { *a = *b + 2;})`. In the code block, operand registers
 * are visible as `a`, `b`, and `c`, all of type `uint32_t *`. Instructions may
 * also access the `Core &core`, `Instruction &inst`, `Program &program`, and
 * `Avidastate &state`.
 */
#define INST(InstName, InstCode)                                               \
  struct InstName {                                                            \
    template <typename Spec>                                                   \
    static void run(sgpl::Core<Spec> &core,                                    \
                    const sgpl::Instruction<Spec> &inst,                       \
                    const sgpl::Program<Spec> &program, CPUState &state) {     \
      uint32_t *a = (uint32_t *)&core.registers[inst.args[0]],                 \
               *b = (uint32_t *)&core.registers[inst.args[1]],                 \
               *c = (uint32_t *)&core.registers[inst.args[2]];                 \
      /* avoid "unused variable" warnings */                                   \
      a = a, b = b, c = c;                                                     \
      InstCode                                                                 \
    }                                                                          \
    static size_t prevalence() { return 1; }                                   \
    static std::string name() { return #InstName; }                            \
  };

INST(JumpIfNEq, {
  // Even != works differently on floats because of NaNs
  if (*a != *b) {
    core.JumpToIndex(state.jump_table[core.GetProgramCounter()]);
  }
});
INST(JumpIfLess, {
  if (*a < *b) {
    core.JumpToIndex(state.jump_table[core.GetProgramCounter()]);
  }
});
INST(Increment, { ++*a; });
INST(Decrement, { --*a; });
// Unary shift (>>1 or <<1)
INST(ShiftLeft, { *a <<= 1; });
INST(ShiftRight, { *a >>= 1; });
INST(Add, { *a = *b + *c; });
INST(Subtract, { *a = *b - *c; });
INST(Nand, { *a = ~(*b & *c); });
INST(Push, {
  // Organism stacks cap out at 16 elements so they don't waste too much memory
  if (state.stack.size() < 16) {
    state.stack.push_back(*a);
  }
});
INST(Pop, {
  if (state.stack.empty()) {
    *a = 0;
  } else {
    *a = state.stack.back();
    state.stack.pop_back();
  }
});
INST(SwapStack, { std::swap(state.stack, state.stack2); });
INST(Swap, { std::swap(*a, *b); });
std::mutex reproduce_mutex;
INST(Reproduce, {
  // Only one reproduction is allowed per update
  if (state.in_progress_repro != -1 || !state.location.IsValid())
    return;
  double points = state.host->IsHost()
                      ? state.world->GetConfig()->HOST_REPRO_RES()
                      : state.world->GetConfig()->SYM_HORIZ_TRANS_RES();
  if (state.host->GetPoints() > points) {
    state.host->AddPoints(-points);
    // Add this organism to the queue to reproduce, using the mutex to avoid a
    // data race
    std::lock_guard<std::mutex> lock(reproduce_mutex);
    state.in_progress_repro = state.world->to_reproduce.size();
    state.world->to_reproduce.push_back(std::pair(state.host, state.location));
  }
});
// Set output to value of register and set register to new input
INST(PrivateIO, {
  float score = state.world->GetTaskSet().CheckTasks(state, *a, false);
  if (score != 0.0) {
    if (!state.host->IsHost()) {
      state.world->GetSymEarnedDataNode().WithMonitor(
          [=](auto &m) { m.AddDatum(score); });
    } else {
      // A host loses 25% of points when performing private IO operations
      score *= 0.75;
    }
    state.host->AddPoints(score);
  }
  uint32_t next;
  if (state.world->GetConfig()->RANDOM_IO_INPUT()){
     next = sgpl::tlrand.Get().GetBits50();
  }else{
      next = 1;
  }
  *a = next;
  state.input_buf.push(next);
});
void AddOrganismPoints(CPUState state, uint32_t output) {
  float score = state.world->GetTaskSet().CheckTasks(state, output, true);
  if (score != 0.0) {
    state.host->AddPoints(score);
    if (!state.host->IsHost()) {
      state.world->GetSymEarnedDataNode().WithMonitor(
          [=](auto &m) { m.AddDatum(score); });
    }
  }
}
// Set output to value of register and set register to new input
INST(SharedIO, {
  AddOrganismPoints(state, *a);
  uint32_t next;
  if (state.world->GetConfig()->RANDOM_IO_INPUT()){
     next = sgpl::tlrand.Get().GetBits50();
  }else{
      next = 1;
  }
  *a = next;
  state.input_buf.push(next);
});
INST(Donate, {
  if (state.world->GetConfig()->DONATION_STEAL_INST()){
  if (state.host->IsHost())
    return;
  if (emp::Ptr<Organism> host = state.host->GetHost()) {
    // Donate 20% of the total points of the symbiont-host system
    // This way, a sym can donate e.g. 40 or 60 percent of their points in a
    // couple of instructions
    double to_donate =
        fmin(state.host->GetPoints(),
             (state.host->GetPoints() + host->GetPoints()) * 0.20);
    state.world->GetSymDonatedDataNode().WithMonitor(
        [=](auto &m) { m.AddDatum(to_donate); });
    host->AddPoints(to_donate * (1.0 - state.world->GetConfig()->DONATE_PENALTY()));
    state.host->AddPoints(-to_donate);
  }
}
});
INST(Steal, {
  if (state.world->GetConfig()->DONATION_STEAL_INST()){
  if (state.host->IsHost())
    return;
  if (emp::Ptr<Organism> host = state.host->GetHost()) {
    // Steal 20% of the total points of the symbiont-host system
    // This way, a sym can steal e.g. 40 or 60 percent of their points in a
    // couple of instructions
    // 10% of the stolen resources are lost
    double to_steal =
        fmin(host->GetPoints(),
             (state.host->GetPoints() + host->GetPoints()) * 0.20);
    state.world->GetSymStolenDataNode().WithMonitor(
        [=](auto &m) { m.AddDatum(to_steal); });
    host->AddPoints(-to_steal);
    state.host->AddPoints(to_steal * (1.0 - state.world->GetConfig()->STEAL_PENALTY()));
  }
  }
});

INST(ReuptakePublic, {
  uint32_t next;
  AddOrganismPoints(state, *a);
  if (state.internalEnvironment->size() > 0) { // Only gets resources if the organism has values in their internal environment
    next = state.internalEnvironment->back(); // Takes a resource from back of internal environment vector
    if (state.host->IsHost()){
      std::cout << "Host : " << next << std::endl;
    }
    state.internalEnvironment->pop_back(); // Clears out the selected resource from Internal Environment
    *a = next;
    state.input_buf.push(next);
  } else {
    // Otherwise, reset the register to 0
    *a = 0;
  }
});
INST(ReuptakePrivate, {
  uint32_t next;
  AddOrganismPoints(state, *a);
  if (state.internalEnvironmentPrivate->size() > 0) { // Only gets resources if the organism has values in their internal environment
    next = state.internalEnvironmentPrivate->back(); // Takes a resource from back of internal environment vector
    if (state.host->IsHost()){
      std::cout << "Host : " << next << std::endl;
    }
    state.internalEnvironmentPrivate->pop_back(); // Clears out the selected resource from Internal Environment
    *a = next;
    state.input_buf.push(next);
  } else {
    // Otherwise, reset the register to 0
    *a = 0;
  }
});
INST(InternalPrivate, {
  state.internalPrivate = true;
});
INST(InternalShared, {
  state.internalPrivate = false;
});

} // namespace inst

#endif