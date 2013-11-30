//===------------------------- typeinfo.cpp -------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is dual licensed under the MIT and the University of Illinois Open
// Source Licenses. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
extern "C" {
#include <stdlib.h>
}
#if __APPLE__
#include <cxxabi.h>
#endif

#include "typeinfo"

std::bad_cast::bad_cast() throw()
{
}

std::bad_cast::~bad_cast() throw()
{
}

const char*
std::bad_cast::what() const throw()
{
  return "std::bad_cast";
}

std::bad_typeid::bad_typeid() throw()
{
}

std::bad_typeid::~bad_typeid() throw()
{
}

const char*
std::bad_typeid::what() const throw()
{
  return "std::bad_typeid";
}

#if __APPLE__
  // On Darwin, the cxa_bad_* functions cannot be in the lower level library
  // because bad_cast and bad_typeid are defined in his higher level library
  void __cxxabiv1::__cxa_bad_typeid() { throw std::bad_typeid(); }
  void __cxxabiv1::__cxa_bad_cast() { throw std::bad_cast(); }
#endif

