#include <iostream>
#include <algorithm>
using namespace std;
typedef struct student {
    int num;
    int score;
}student;
bool compare(student a, student b) {
    return a.num < b.num;
}
int main () {
    int m,n;
    cin >> m >> n;
    int sum = m+n;
    student a[sum];
    for (int i = 0; i < m; i++) {
        cin >> a[i].num >> a[i].score;
    }
    for (int i = m; i < sum; i++) {
        cin >> a[i].num >> a[i].score;
    }
    sort(a, a+sum, compare);
    for (int i = 0; i < sum; i++) {
        cout << a[i].num << " " << a[i].score << endl;
    }
}