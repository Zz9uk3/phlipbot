#include <string>

#define DOCTEST_CONFIG_IMPLEMENTATION_IN_DLL
#include <doctest.h>

#include <boost/exception/exception.hpp>
#include <boost/exception/diagnostic_information.hpp>

doctest::String boost_exception_translator(boost::exception const& ex)
{
  return boost::diagnostic_information(ex).c_str();
}

// Run doctest-style unittests
//
// The actual tests themselves are written in-source in the phlipbot/ project,
// this test runner just links phlipbot.dll and runs the doctest_runner.cpp
// test runner defined in phlipbot.dll

int main(int argc, char* argv[])
{
  doctest::registerExceptionTranslator(&boost_exception_translator);
  doctest::Context context{argc, argv};
  int res = context.run();

  if (context.shouldExit()) {
    return res;
  }

  return res;
}
