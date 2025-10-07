// src/evaluator/Evaluator.cpp
#include "evaluator.hpp"
#include "ClassRuntime.hpp"
#include "globals.hpp"
#include <iostream>
#include <cmath>
#include <stdexcept>
#include <sstream>

Evaluator::Evaluator(): global_env(std::make_shared < Environment > (nullptr)) {
  global_env = std::make_shared < Environment > ();
  init_globals(global_env);
}


// ----------------- Program evaluation -----------------
void Evaluator::evaluate(ProgramNode* program) {
  if (!program) return;
  Value dummy_ret;
  bool did_return = false;
  for (auto &stmt_uptr: program->body) {
    evaluate_statement(stmt_uptr.get(), global_env, &dummy_ret, &did_return);
    if (did_return) break;
  }
  try {
    run_event_loop();
  } catch (const std::exception &e) {
    std::cerr << "Error while running async callbacks: " << e.what() << std::endl;
  } catch (...) {
    std::cerr << "Unknown error while running async callbacks\n";
  }

}