#pragma once
// Compatibility only: original test bodies and assertions remain unedited.
#include "contract.hpp"
#define BOOST_AUTO_TEST_CASE(name) UNIT_CASE(upstream,name)
#define BOOST_CHECK(expr) UNIT_CHECK(expr)
#define BOOST_REQUIRE(expr) UNIT_CHECK(expr)
#define BOOST_CHECK_EQUAL(a,b) UNIT_CHECK((a)==(b))
#define BOOST_REQUIRE_EQUAL(a,b) UNIT_CHECK((a)==(b))
#define BOOST_CHECK_MESSAGE(expr,message) UNIT_CHECK(expr)
#define BOOST_CHECK_NO_THROW(expr) do { bool unit_ok=true; try { expr; } catch(const std::exception& unit_e) { std::cerr<<unit_e.what()<<"\n"; unit_ok=false; } UNIT_CHECK(unit_ok); } while(false)
