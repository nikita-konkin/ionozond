# Differential test: call getMedian() inside the ORIGINAL binary and print the
# result, so the reconstruction can be compared against it directly.
set confirm off
set pagination off
break main
run
# 8 values -> even n
set $getMedian = (double (*)(double*, int*))getMedian
set $a = (double*)malloc(8*8)
set $a[0] = 5.0
set $a[1] = 1.0
set $a[2] = 9.0
set $a[3] = 3.0
set $a[4] = 7.0
set $a[5] = 2.0
set $a[6] = 8.0
set $a[7] = 4.0
set $n = (int*)malloc(4)
set *$n = 8
printf "getMedian(even n=8)  = %.17g\n", $getMedian($a, $n)
# 7 values -> odd n, exposes the missing parity branch
set *$n = 7
printf "getMedian(odd  n=7)  = %.17g\n", $getMedian($a, $n)
set *$n = 5
printf "getMedian(odd  n=5)  = %.17g\n", $getMedian($a, $n)
set *$n = 2
printf "getMedian(n=2)       = %.17g\n", $getMedian($a, $n)
kill
quit
