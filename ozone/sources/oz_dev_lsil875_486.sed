#
#  Turn the global read/write arrays into static read-only arrays
#
sed "s/^ULONG/static const uLong/g" oz_dev_lsil875_486.out > x.x
sed "s/^char/static const char/g" x.x > oz_dev_lsil875_486.out
rm -f x.x
