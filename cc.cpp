#include <iostream>
using namespace std;

// 定义一个常量，表示我们用于存储大数的数组的最大长度。
// 50!大约有65位，1000的容量对于计算50!绰绰有余。
const int MAX_DIGITS = 1000;

// 高精度阶乘计算函数
void bigIntegerFactorial(int n) {
    int result[MAX_DIGITS] = {0}; // 初始化一个数组用于存储结果，所有位初始为0
    result[0] = 1;              // 阶乘从1开始
    int resultSize = 1;          // 记录当前结果占用的数组长度，初始为1（即只有个位是1）

    // 外层循环：从2乘到n，计算n!
    for (int currentNum = 2; currentNum <= n; currentNum++) {
        int carry = 0; // 进位初始为0

        // 内层循环：将当前结果（存储在result数组中）的每一位与currentNum相乘
        for (int digitPos = 0; digitPos < resultSize; digitPos++) {
            int product = result[digitPos] * currentNum + carry; // 计算当前位的乘积并加上进位
            result[digitPos] = product % 10; // 结果的当前位是乘积的个位数
            carry = product / 10;           // 产生的进位是乘积除以10的整数部分
        }

        // 处理内层循环结束后可能剩余的进位
        while (carry) {
            result[resultSize] = carry % 10; // 将进位的个位数作为新的最高位
            carry = carry / 10;              // 继续处理进位的高位
            resultSize++;                     // 结果的长度增加
        }
    }

    // 输出结果：因为数组是逆序存储的，所以需要从后往前输出（从最高位到最低位）
    cout << n << "! = ";
    for (int i = resultSize - 1; i >= 0; i--) {
        cout << result[i];
    }
    cout << endl;
}

int main() {
    int n;
    cout << "请输入一个正整数，计算其阶乘: ";
    cin >> n;

    if (n < 0) {
        cout << "错误：阶乘没有负数的定义。" << endl;
    } else {
        bigIntegerFactorial(n);
    }

    return 0;
}