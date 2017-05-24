#! /usr/bin/perl

# Test of hmmbuild/nhmmer as used to build a DNA model, then query a  
# a database of long (1MB).
#
# Usage:   ./i18-nhmmer-generic.pl <builddir> <srcdir> <tmpfile prefix>
# Example: ./i18-nhmmer-generic.pl ..         ..       tmpfoo
#
# TJW, Fri Nov 12 11:07:31 EST 2010 [Janelia]

BEGIN {
    $builddir  = shift;
    $srcdir    = shift;
    $tmppfx    = shift;
    $verbose   = shift;  # if arg not given, defaults to false (zero)
}


# The test makes use of the following file:
#
# 3box.sto              <msafile>  Single 3box alignment

# It creates the following files:
# $tmppfx.hmm           <hmm>     1 model, 3box
# $tmppfx.A             <seqfile> 1 random seq, ~4.5MB in length
# $tmppfx.B             <seqfile> 2 random seqs, generated by hmmemit from $tmppfx.hmm 
# $tmppfx.fa            <seqdb>   Roughly 4.5MB of a single pseudochromosome, consisting of the two sequences from $tmppfx.B inserted into the sequence of $tmppfx.A  

# All models assumed to be in testsuite subdirectory.
$alignment   = "3box.sto";

@h3progs =  ( "hmmemit", "hmmbuild", "nhmmer");
@eslprogs =  ("esl-shuffle");

# Verify that we have all the executables and datafiles we need for the test.
foreach $h3prog  (@h3progs)  { if (! -x "$builddir/src/$h3prog")              { die "FAIL: didn't find $h3prog executable in $builddir/src\n";              } }
foreach $eslprog (@eslprogs) { if (! -x "$builddir/easel/miniapps/$eslprog")  { die "FAIL: didn't find $eslprog executable in $builddir/easel/miniapps\n";  } }

if (! -r "$srcdir/testsuite/$alignment")  { die "FAIL: can't read msa $alignment in $srcdir/testsuite\n"; }

# Create the test hmm
$cmd = "$builddir/src/hmmbuild $tmppfx.hmm $srcdir/testsuite/$alignment";
$output = do_cmd($cmd);
if ($? != 0) { die "FAIL: hmmbuild failed unexpectedly\n"; } 
if ($output !~ /1     3box                    22    22    20    75    22.00  1.415/) {
	die "FAIL: hmmbuild failed to build correctly\n";
}
$output = do_cmd( "grep MAXL $tmppfx.hmm" );
if ($output !~ /MAXL  75/) {
    die "FAIL: hmmbuild failed to build correctly\n";
}


# Create a roughly 4.5MB database against which to search
$database   = "$tmppfx.fa";
do_cmd ( "$builddir/easel/miniapps/esl-shuffle --seed 33 --dna -G -N 1 -L 4500000 -o $tmppfx.A" );
do_cmd ( "$builddir/src/hmmemit -N 2 --seed 4 $tmppfx.hmm >  $tmppfx.B " );
do_cmd ( "$builddir/src/hmmemit -N 1 --seed 3 $tmppfx.hmm >> $tmppfx.B" ); 
do_cmd ( "head -n 33000 $tmppfx.A > $database" );
do_cmd ( "head -n 2 $tmppfx.B | tail -n 1 >> $database" );
do_cmd ( "tail -n +33001 $tmppfx.A | head -n 22000 >> $database");
do_cmd ( "head -n 4 $tmppfx.B | tail -n 1 >> $database" );
do_cmd ( "tail -n 20000 $tmppfx.A >> $database" );
do_cmd ( "tail -n 1 $tmppfx.B >> $database" );

# perform nhmmer search
$cmd = "$builddir/src/nhmmer --tformat fasta $tmppfx.hmm $database";
$output = do_cmd($cmd);

if ($? != 0) { die "FAIL: nhmmer failed unexpectedly\n"; }
$expect = 
q[
Target sequences:                            1  \(9000000 residues searched\)
Residues passing SSV filter:            174175  \(0.0194\); expected \(0.02\)
Residues passing bias filter:           145307  \(0.0161\); expected \(0.02\)
Residues passing Vit filter:             18396  \(0.00204\); expected \(0.003\)
Residues passing Fwd filter:               415  \(4.61e-05\); expected \(3e-05\)
Total number of hits:                        4  \(8e-06\)];
if ($output !~ /$expect/s) {
    die "FAIL: nhmmer failed search test 1\n";
}

$expect =   q[
       0.45   13.6   5.2  random   4499980 4499998\s+
        1.7   11.8   4.4  random   1979941 1979960\s+
        1.8   11.7   4.2  random   3299961 3299978\s+
        8.2    9.7   2.3  random   2354832 2354818]; 
if ($output !~ /$expect/s) {
    die "FAIL: nhmmer failed search test 2\n";
}

$cmd = "$builddir/src/nhmmer --tformat fasta --watson $tmppfx.hmm $database";
$output = do_cmd($cmd);
if ($? != 0) { die "FAIL: nhmmer failed unexpectedly\n"; }
$expect = 
q[
Target sequences:                            1  \(4500000 residues searched\)
Residues passing SSV filter:             84366  \(0.0187\); expected \(0.02\)
Residues passing bias filter:            70869  \(0.0157\); expected \(0.02\)
Residues passing Vit filter:              9070  \(0.00202\); expected \(0.003\)
Residues passing Fwd filter:               228  \(5.07e-05\); expected \(3e-05\)
Total number of hits:                        3  \(1.27e-05\)];


if ($output !~ /$expect/s) {
    die "FAIL: nhmmer failed search test 3\n";
}
$expect = 
     q[
       0.22   13.6   5.2  random   4499980 4499998\s+
       0.86   11.8   4.4  random   1979941 1979960\s+
       0.89   11.7   4.2  random   3299961 3299978]; 

if ($output !~ /$expect/s) {
    die "FAIL: nhmmer failed search test 4\n";
}

print "ok.\n";
unlink "$tmppfx.hmm";
unlink "$tmppfx.A";
unlink "$tmppfx.B";
unlink "$tmppfx.fa";

exit 0;


sub do_cmd {
    $cmd = shift;
    print "$cmd\n" if $verbose;
    return `$cmd`;	
}
