#!/bin/bash
#
# elfc.bash <image> <object> [-dynamic]
#
option="$2"
outdir="../binaries"
outext="oz"
shares="oz_kernel_486.oz"
if [ "$1" = "oz_kernel_486" ]
then
  shares=""
fi
if [ "$1" = "oz_kernel_smp_486" ]
then
  shares=""
fi
if [ "$1" = "oz_ldr_expand_486" ]
then
  outdir="../objects"
  outext="raw"
  shares=""
fi
if [ "$1" = "oz_loader_486" ]
then
  outdir="../objects"
  outext="raw"
  shares=""
fi
if [ "$1" = "oz_memtest_486" ]
then
  outdir="../objects"
  outext="raw"
  shares=""
fi
echo oz_util_elfconv $option $outdir/$1.$outext ../objects/$1.r $shares
ln -s -f ../binaries/oz_kernel_486.oz oz_kernel_486.oz
../linux/oz_util_elfconv $option $outdir/$1.$outext ../objects/$1.r $shares | sort > ../objects/$1.ms
status=$PIPESTATUS
rm -f oz_kernel_486.oz
exit $status
