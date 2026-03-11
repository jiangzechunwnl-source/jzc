#include <iostream>
#include <iomanip>
using namespace std;

int main() {
    double sum = 0.0;
    double temp = 1.0;

    for (int i = 1; i <= 30; i++) {
        temp *= i;
        sum += temp;
    }

    cout << fixed << setprecision(2) << scientific << sum << endl;

    return 0;
}