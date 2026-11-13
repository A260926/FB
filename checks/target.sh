set -eu
cd "$(dirname "$0")/.."
test -f checks/stack_asan.cpp
test -f checks/stack_asan.sh
bash checks/stack_asan.sh
bash checks/install.sh
bash checks/baseline.sh
