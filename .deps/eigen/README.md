**Eigen is a C++ template library for linear algebra: matrices, vectors, numerical solvers, and related algorithms.**

For more information go to http://eigen.tuxfamily.org/ or https://libeigen.gitlab.io.

For ***pull request***, ***bug reports***, and ***feature requests***, go to https://gitlab.com/libeigen/eigen.

<br>

## Installation

Run:

```bash
$ npm i eigen.cxx
```

And then include `Eigen.h` as follows:

```cxx
// main.cxx
#include <Eigen.h>  // or, Eigen/Eigen

int main() { /* ... */ }
```

Finally, compile while adding the path `node_modules/eigen.cxx` to your compiler's include paths.

```bash
$ clang++ -I./node_modules/eigen.cxx main.cxx  # or, use g++
$ g++     -I./node_modules/eigen.cxx main.cxx
```

You may also use a simpler approach with the [cpoach](https://www.npmjs.com/package/cpoach.sh) tool, which automatically adds the necessary include paths of all the installed dependencies for your project.

```bash
$ cpoach clang++ main.cxx  # or, use g++
$ cpoach g++     main.cxx
```

<br>
<br>


[![](https://raw.githubusercontent.com/qb40/designs/gh-pages/0/image/11.png)](https://wolfram77.github.io)<br>
[![SRC](https://img.shields.io/badge/src-repo-green?logo=Org)](https://gitlab.com/libeigen/eigen)
[![ORG](https://img.shields.io/badge/org-nodef-green?logo=Org)](https://nodef.github.io)
![](https://ga-beacon.deno.dev/G-RC63DPBH3P:SH3Eq-NoQ9mwgYeHWxu7cw/github.com/nodef/eigen.cxx)
