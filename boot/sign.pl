#!/usr/bin/perl

# Pads the raw boot sector out to exactly 512 bytes and appends the
# 0x55AA signature the BIOS looks for at the end of the first sector,
# in order to identify it as bootable and load it into memory.

open(SIG, $ARGV[0]) || die "open $ARGV[0]: $!";

$n = sysread(SIG, $buf, 1000);

if($n > 510){
  print STDERR "boot block too large: $n bytes (max 510)\n";
  exit 1;
}

print STDERR "boot block is $n bytes (max 510)\n";

$buf .= "\0" x (510-$n);
$buf .= "\x55\xAA";

open(SIG, ">$ARGV[0]") || die "open >$ARGV[0]: $!";
print SIG $buf;
close SIG;
