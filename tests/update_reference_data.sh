#!/bin/bash

OPM_TESTS_ROOT=$1
TMPDIR=`mktemp -d`
mkdir $TMPDIR/orig
mkdir $TMPDIR/new

# Copy results from a test run to refence dir
# $1 = source directory to copy data from
# $2 = destination directory to copy data to
# $3 = base file name for files to copy
# $4...$@ = file types to copy
copyToReferenceDir () {
  SRC_DIR=$1
  DST_DIR=$2
  STEM=$3
  FILETYPES=${@:4}
  mkdir -p $DST_DIR

  DIFF=1
  for filetype in $FILETYPES
  do
    # Don't flag as changed if both reference and result dir lack a file type
    # In particular to handle the optional RFT's
    if [ ! -f $WORKSPACE/$SRC_DIR$STEM.$filetype ] && [ ! -f $DST_DIR/$STEM.$filetype ]
    then
      continue
    fi
    diff -q "$WORKSPACE/$SRC_DIR$STEM.$filetype" "$DST_DIR/$STEM.$filetype"
    if test $? -ne 0
    then
      cp $WORKSPACE/$SRC_DIR$STEM.$filetype $TMPDIR/new
      $configuration/install/bin/convertECL $TMPDIR/new/$STEM.$filetype
      cp $DST_DIR/$STEM.$filetype $TMPDIR/orig
      $configuration/install/bin/convertECL $TMPDIR/orig/$STEM.$filetype
      diff -u $TMPDIR/orig/$STEM.F$filetype $TMPDIR/new/$STEM.F$filetype >> $WORKSPACE/data_diff
      cp "$WORKSPACE/$SRC_DIR$STEM.$filetype" $DST_DIR
      DIFF=0
    fi
  done

  return $DIFF
}

declare -A tests
# The tests are listed in a dictionary mapping name of the test to the commands
# used to run the test:
#
#    tests[cmake_testname]="binary dirname SIMULATION_CASE
#
# The key in the dictionary must agree with the name given to the test when
# registering it with cmake in compareEclFiles.cmake. The SIMULATION_CASE should
# be the basename of the .DATA file used for the simulation.
tests[spe1]="flow spe1 SPE1CASE1"
tests[spe12]="flow spe1 SPE1CASE2"
tests[spe1_2p]="flow spe1 SPE1CASE2_2P"
tests[spe1_oilgas]="flow spe1 SPE1CASE2_OILGAS"
tests[spe1_gaswater]="flow spe1 SPE1CASE2_GASWATER"
tests[spe1_nowells]="flow spe1 SPE1CASE2_NOWELLS"
tests[spe1_thermal]="flow spe1 SPE1CASE2_THERMAL"
tests[spe1_thermal_onephase]="flow_onephase_energy spe1 SPE1CASE2_THERMAL_ONEPHASE"
tests[spe1_thermal_watvisc]="flow spe1 SPE1CASE2_THERMAL_WATVISC"
tests[spe1_rockcomp]="flow spe1 SPE1CASE2_ROCK2DTR"
tests[spe1_brine]="flow spe1_brine SPE1CASE1_BRINE"
tests[spe1_water]="flow_onephase spe1 SPE1CASE1_WATER"
tests[spe1_spider]="flow radial_grid SPIDER_CAKESLICE"
tests[spe1_import]="flow spe1 SPE1CASE1_IMPORT"
tests[ctaquifer_2d_oilwater]="flow aquifer-oilwater 2D_OW_CTAQUIFER"
tests[fetkovich_2d]="flow aquifer-fetkovich 2D_FETKOVICHAQUIFER"
tests[numerical_aquifer_3d_1aqu]="flow aquifer-num 3D_1AQU_3CELLS"
tests[numerical_aquifer_3d_2aqu]="flow aquifer-num 3D_2AQU_NUM"
tests[msw_2d_h]="flow msw_2d_h 2D_H__"
tests[msw_3d_hfa]="flow msw_3d_hfa 3D_MSW"
tests[polymer_oilwater]="flow polymer_oilwater 2D_OILWATER_POLYMER"
tests[polymer_simple2D]="flow polymer_simple2D 2D_THREEPHASE_POLY_HETER"
tests[spe3]="flow spe3 SPE3CASE1"
tests[spe5]="flow spe5 SPE5CASE1"
tests[spe5_co2eor]="flow spe5_co2eor SPE5CASE1_DYN"
tests[spe9group]="flow spe9group SPE9_CP_GROUP"
tests[spe9group_resv]="flow spe9group SPE9_CP_GROUP_RESV"
tests[spe9]="flow spe9 SPE9_CP_SHORT"
tests[wecon_wtest]="flow wecon_wtest 3D_WECON"
tests[spe1_metric_vfp1]="flow vfpprod_spe1 SPE1CASE1_METRIC_VFP1"
tests[base_model_1]="flow model1 BASE_MODEL_1"
tests[msw_model_1]="flow model1 MSW_MODEL_1"
tests[faults_model_1]="flow model1 FAULTS_MODEL_1"
tests[base_model2]="flow model2 0_BASE_MODEL2"
tests[0a1_grpctl_stw_model2]="flow model2 0A1_GRCTRL_LRAT_ORAT_BASE_MODEL2_STW"
tests[0a1_grpctl_msw_model2]="flow model2 0A1_GRCTRL_LRAT_ORAT_BASE_MODEL2_MSW"
tests[0a2_grpctl_stw_model2]="flow model2 0A2_GRCTRL_LRAT_ORAT_GGR_BASE_MODEL2_STW"
tests[0a2_grpctl_msw_model2]="flow model2 0A2_GRCTRL_LRAT_ORAT_GGR_BASE_MODEL2_MSW"
tests[0a3_grpctl_stw_model2]="flow model2 0A3_GRCTRL_LRAT_LRAT_BASE_MODEL2_STW"
tests[0a3_grpctl_msw_model2]="flow model2 0A3_GRCTRL_LRAT_LRAT_BASE_MODEL2_MSW"
tests[0a4_grpctl_stw_model2]="flow model2 0A4_GRCTRL_LRAT_LRAT_GGR_BASE_MODEL2_STW"
tests[0a4_grpctl_msw_model2]="flow model2 0A4_GRCTRL_LRAT_LRAT_GGR_BASE_MODEL2_MSW"
tests[multregt_model2]="flow model2 1_MULTREGT_MODEL2"
tests[multxyz_model2]="flow model2 2_MULTXYZ_MODEL2"
tests[multflt_model2]="flow model2 3_MULTFLT_MODEL2"
tests[multpvv_model2]="flow model2 4_MINPVV_MODEL2"
tests[swatinit_model2]="flow model2 5_SWATINIT_MODEL2"
tests[endscale_model2]="flow model2 6_ENDSCALE_MODEL2"
tests[hysteresis_model2]="flow model2 7_HYSTERESIS_MODEL2"
tests[multiply_tranxyz_model2]="flow model2 8_MULTIPLY_TRANXYZ_MODEL2"
tests[editnnc_model2]="flow model2 9_EDITNNC_MODEL2"
tests[9_1a_grpctl_stw_model2]="flow model2 9_1A_DEPL_MAX_RATE_MIN_BHP_STW"
tests[9_1a_grpctl_msw_model2]="flow model2 9_1A_DEPL_MAX_RATE_MIN_BHP_MSW"
tests[9_1b_grpctl_stw_model2]="flow model2 9_1B_DEPL_MAX_RATE_MIN_THP_STW"
tests[9_1b_grpctl_msw_model2]="flow model2 9_1B_DEPL_MAX_RATE_MIN_THP_MSW"
tests[9_2a_grpctl_stw_model2]="flow model2 9_2A_DEPL_GCONPROD_1L_STW"
tests[9_2a_grpctl_msw_model2]="flow model2 9_2A_DEPL_GCONPROD_1L_MSW"
tests[9_2b_grpctl_stw_model2]="flow model2 9_2B_DEPL_GCONPROD_2L_STW"
tests[9_2b_grpctl_msw_model2]="flow model2 9_2B_DEPL_GCONPROD_2L_MSW"
tests[9_3a_grpctl_stw_model2]="flow model2 9_3A_GINJ_REIN-G_STW"
tests[9_3a_grpctl_msw_model2]="flow model2 9_3A_GINJ_REIN-G_MSW"
tests[9_3b_grpctl_stw_model2]="flow model2 9_3B_GINJ_GAS_EXPORT_STW"
tests[9_3b_grpctl_msw_model2]="flow model2 9_3B_GINJ_GAS_EXPORT_MSW"
tests[9_3c_grpctl_stw_model2]="flow model2 9_3C_GINJ_GAS_GCONSUMP_STW"
tests[9_3c_grpctl_msw_model2]="flow model2 9_3C_GINJ_GAS_GCONSUMP_MSW"
tests[9_3d_grpctl_stw_model2]="flow model2 9_3D_GINJ_GAS_MAX_EXPORT_STW"
tests[9_3d_grpctl_msw_model2]="flow model2 9_3D_GINJ_GAS_MAX_EXPORT_MSW"
tests[9_3e_grpctl_stw_model2]="flow model2 9_3E_GAS_MIN_EXPORT_STW"
tests[9_3e_grpctl_msw_model2]="flow model2 9_3E_GAS_MIN_EXPORT_MSW"
tests[9_4a_grpctl_stw_model2]="flow model2 9_4A_WINJ_MAXWRATES_MAXBHP_GCONPROD_1L_STW"
tests[9_4a_grpctl_msw_model2]="flow model2 9_4A_WINJ_MAXWRATES_MAXBHP_GCONPROD_1L_MSW"
tests[9_4b_grpctl_stw_model2]="flow model2 9_4B_WINJ_VREP-W_STW"
tests[9_4b_grpctl_msw_model2]="flow model2 9_4B_WINJ_VREP-W_MSW"
tests[9_4c_grpctl_stw_model2]="flow model2 9_4C_WINJ_GINJ_VREP-W_REIN-G_STW"
tests[9_4c_grpctl_msw_model2]="flow model2 9_4C_WINJ_GINJ_VREP-W_REIN-G_MSW"
tests[9_4d_grpctl_stw_model2]="flow model2 9_4D_WINJ_GINJ_GAS_EXPORT_STW"
tests[9_4d_grpctl_msw_model2]="flow model2 9_4D_WINJ_GINJ_GAS_EXPORT_MSW"
tests[model4_group]="flow model4 MOD4_GRP"
tests[model4_udq_group]="flow model4 MOD4_UDQ_ACTIONX"
tests[polymer_injectivity]="flow polymer_injectivity 2D_POLYMER_INJECTIVITY"
tests[nnc]="flow editnnc NNC_AND_EDITNNC"
tests[udq_wconprod]="flow udq_actionx UDQ_WCONPROD"
tests[udq_actionx]="flow udq_actionx UDQ_ACTIONX"
tests[actionx_m1]="flow udq_actionx ACTIONX_M1"
tests[udq_uadd]="flow udq_actionx UDQ_M1"
tests[udq_undefined]="flow udq_actionx UDQ_M2"
tests[udq_in_actionx]="flow udq_actionx UDQ_M3"
tests[udq_pyaction]="flow udq_actionx PYACTION_WCONPROD"
tests[spe1_foam]="flow spe1_foam SPE1FOAM"
tests[wsegsicd]="flow wsegsicd TEST_WSEGSICD"
tests[wsegaicd]="flow wsegaicd BASE_MSW_WSEGAICD"
tests[bc_lab]="flow bc_lab BC_LAB"
tests[pinch_multz_all]="flow pinch PINCH_MULTZ_ALL"
tests[pinch_multz_all_barrier]="flow pinch PINCH_MULTZ_ALL_BARRIER"
tests[model6_msw]="flow model6 1_MSW_MODEL6"
tests[norne_reperf]="flow norne NORNE_ATW2013_B1H_RE-PERF"
tests[compl_smry]="flow compl_smry COMPL_SMRY"
tests[3d_tran_operator]="flow parallel_fieldprops 3D_TRAN_OPERATOR"
tests[co2store]="flow co2store CO2STORE"
tests[co2store_diffusive]="flow co2store CO2STORE_DIFFUSIVE"
tests[co2store_drsdtcon]="flow co2store CO2STORE_DRSDTCON"

changed_tests=""

# Read failed tests
FAILED_TESTS=`cat $WORKSPACE/$configuration/build-opm-simulators/Testing/Temporary/LastTestsFailed*.log`

test -z "$FAILED_TESTS" && exit 5

for failed_test in $FAILED_TESTS
do
  failed=`echo $failed_test | sed -e 's/.*:compareECLFiles_//g'`
  for test_name in ${!tests[*]}
  do
    binary=`echo ${tests[$test_name]} | awk -F ' ' '{print $1}'`
    dirname=`echo ${tests[$test_name]} | awk -F ' ' '{print $2}'`
    casename=`echo ${tests[$test_name]} | awk -F ' ' '{print $3}'`
    if grep -q "$failed" <<< "$binary+$casename"
    then
      copyToReferenceDir \
          $configuration/build-opm-simulators/tests/results/$binary+$test_name/ \
          $OPM_TESTS_ROOT/$dirname/opm-simulation-reference/$binary \
          $casename \
          EGRID INIT RFT SMSPEC UNRST UNSMRY
      test $? -eq 0 && changed_tests="$changed_tests $test_name"
    fi
  done
done

# special tests
copyToReferenceDir \
      $configuration/build-opm-simulators/tests/results/init/flow+norne/ \
      $OPM_TESTS_ROOT/norne/opm-simulation-reference/flow \
      NORNE_ATW2013 \
      EGRID INIT
test $? -eq 0 && changed_tests="$changed_tests norne_init"

changed_tests=`echo $changed_tests | xargs`
echo -e "Automatic Reference Data Update for PR ${REASON:-(Unknown)}\n" > /tmp/cmsg
if [ -z "$REASON" ]
then
  echo -e "Reason: fill in this\n" >> /tmp/cmsg
else
  echo -e "Reason: $REASON\n" >> /tmp/cmsg
fi
for dep in opm-common opm-grid opm-material opm-models
do
  pushd $WORKSPACE/deps/$dep > /dev/null
  name=`printf "%-14s" $dep`
  rev=`git rev-parse HEAD`
  echo -e "$name = $rev" >> /tmp/cmsg
  popd > /dev/null
done
echo -e "opm-simulators = `git rev-parse HEAD`" >> /tmp/cmsg

echo -e "\n### Changed Tests ###\n" >> /tmp/cmsg
for t in ${changed_tests}
do
  echo "  * ${t}" >> /tmp/cmsg
done

cd $OPM_TESTS_ROOT
if [ -n "$BRANCH_NAME" ]
then
  git checkout -b $BRANCH_NAME origin/master
fi

# Add potential new files
untracked=`git status | sed '1,/Untracked files/d' | tail -n +3 | head -n -2`
if [ -n "$untracked" ]
then
  git add $untracked
fi

if [ -z "$REASON" ]
then
  git commit -a -t /tmp/cmsg
else
  git commit -a -F /tmp/cmsg
fi

rm -rf $TMPDIR
