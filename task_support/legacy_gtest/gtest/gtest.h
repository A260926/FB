// Minimal local adapter for the two unchanged split_copy tests only.
#pragma once
#include <functional>
#include <stdexcept>
#include <vector>
namespace testing { class Test { protected: virtual void SetUp(){} virtual void TearDown(){} public: virtual ~Test(){} }; }
namespace legacy {
struct Entry {const char* name;std::function<void()> run;};
inline std::vector<Entry>& entries(){static std::vector<Entry> e;return e;}
inline int& checks(){static int count=0;return count;}
struct Check {template<class A,class B> Check(const A&a,const B&b){++checks();if(!(a==b))throw std::runtime_error("legacy EXPECT_EQ");}template<class T>Check& operator<<(const T&){return *this;}};
}
#define EXPECT_EQ(a,b) legacy::Check((a),(b))
#define TEST_F(F,N) class F##_##N:public F{public:void body();static void run(){F##_##N t;t.SetUp();t.body();t.TearDown();}}; static const bool registered_##F##_##N=[](){legacy::entries().push_back({#F "." #N,&F##_##N::run});return true;}(); void F##_##N::body()
