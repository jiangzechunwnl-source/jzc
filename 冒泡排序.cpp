void sort(int a[], int n) {
	// 标准的冒泡排序（升序）
	for(int i = 0; i < n - 1; i++) {
		for(int j = 0; j < n - 1 - i; j++) {
			if(a[j] > a[j + 1]) {  // 比较相邻元素
				// 交换位置
				int temp = a[j];
				a[j] = a[j + 1];
				a[j + 1] = temp;
			}
		}
	}
}