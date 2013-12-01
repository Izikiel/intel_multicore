#!/bin/bash
#
# elfc_axp.bash <image> <object> [-dynamic]
#
option="$2"
outdir="../alphabin"
outext="oz"
shares="oz_kernel_axp.oz"
if [ "$1" = "oz_kernel_axp" ]
then
  shares=""
fi
if [ "$1" = "oz_kernel_smp_axp" ]
then
  shares=""
fi
if [ "$1" = "oz_loader_axp" ]
then
  outdir="../alphaobj"
  outext="raw"
  shares=""
fi
echo oz_util_elfconv $option $outdir/$1.$outext ../alphaobj/$1.r $shares
ln -s -f ../alphabin/oz_kernel_axp.oz oz_kernel_axp.oz
../linux/oz_util_elfconv_axp $option $outdir/$1.$outext ../alphaobj/$1.r $shares | sort > ../alphaobj/$1.ms
status=$PIPESTATUS
rm -f oz_kernel_axp.oz
exit $status
