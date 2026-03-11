#include <iomanip>
#include <iostream>
#include <algorithm>
#include <bitset>
#include <cstring>
#include <string>
#include <vector>
using namespace std;


int main () {
    int num;cin >> num;
    cout << bitset<8>(num) << endl; //二进制
    cout << oct<< num << endl; //八进制
    cout << hex << num << endl; // 十六进制
    cout << dec << num << endl; // 十进制
}