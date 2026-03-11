#include <inttypes.h>
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdarg>
using namespace std;
int sum(int n, ...) {
    int sum = 0;
    va_list argptr;
    va_start(argptr, n);
    for (int i = 0; i < n; i++) {
        sum += va_arg(argptr, int);
    }
    va_end(argptr);
    return sum;
}
int main() {
    int n = sum(3, 1, 2, 3);
    cout << n << endl;
}