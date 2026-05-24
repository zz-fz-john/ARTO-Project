#pragma once
#include <llvm/Support/raw_ostream.h>

#include <iostream>

#define RESET   "\033[0m"
#define RED     "\033[31m"
#define YELLOW  "\033[33m"

#define INFO(...) (std::cout << "[INFO] " << __VA_ARGS__ << std::endl)
#define WARNING(...) (std::cout << YELLOW << "[WARNING]" << RESET << " " << __VA_ARGS__ << std::endl)
#define ERROR(...) (std::cerr << RED << "[ERROR]" << RESET << " " << __VA_ARGS__ << std::endl)

#define LLVM_INFO(...) (llvm::outs() << "[INFO] " << __VA_ARGS__ << "\n")
#define LLVM_WARNING(...) (llvm::outs() << YELLOW << "[WARNING]" << RESET << " " << __VA_ARGS__ << "\n")
#define LLVM_ERROR(...) (llvm::outs() << RED << "[ERROR]" << RESET << " " << __VA_ARGS__ << "\n")