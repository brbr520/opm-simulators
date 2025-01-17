/*
  Copyright 2017 SINTEF Digital, Mathematics and Cybernetics.
  Copyright 2017 Statoil ASA.
  Copyright 2016 - 2017 IRIS AS.

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <opm/common/utility/numeric/RootFinders.hpp>
#include <opm/parser/eclipse/EclipseState/Schedule/Well/WellInjectionProperties.hpp>
#include <opm/simulators/utils/DeferredLoggingErrorHelpers.hpp>
#include <opm/simulators/linalg/MatrixBlock.hpp>
#include <opm/simulators/wells/VFPHelpers.hpp>

#include <algorithm>
#include <functional>
#include <numeric>

namespace Opm
{

    template<typename TypeTag>
    StandardWell<TypeTag>::
    StandardWell(const Well& well,
                 const ParallelWellInfo& pw_info,
                 const int time_step,
                 const ModelParameters& param,
                 const RateConverterType& rate_converter,
                 const int pvtRegionIdx,
                 const int num_components,
                 const int num_phases,
                 const int index_of_well,
                 const std::vector<PerforationData>& perf_data)
    : Base(well, pw_info, time_step, param, rate_converter, pvtRegionIdx, num_components, num_phases, index_of_well, perf_data)
    , StdWellEval(static_cast<const WellInterfaceIndices<FluidSystem,Indices,Scalar>&>(*this))
    {
        assert(num_components_ == numWellConservationEq);
    }





    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    init(const PhaseUsage* phase_usage_arg,
         const std::vector<double>& depth_arg,
         const double gravity_arg,
         const int num_cells,
         const std::vector< Scalar >& B_avg)
    {
        Base::init(phase_usage_arg, depth_arg, gravity_arg, num_cells, B_avg);
        this->StdWellEval::init(perf_depth_, depth_arg, num_cells, Base::has_polymermw);
    }





    template<typename TypeTag>
    void StandardWell<TypeTag>::
    initPrimaryVariablesEvaluation() const
    {
        this->StdWellEval::initPrimaryVariablesEvaluation();
    }





    template<typename TypeTag>
    typename StandardWell<TypeTag>::Eval
    StandardWell<TypeTag>::getPerfCellPressure(const typename StandardWell<TypeTag>::FluidState& fs) const
    {
        Eval pressure;
        if (Indices::oilEnabled) {
            pressure = fs.pressure(FluidSystem::oilPhaseIdx);
        } else {
            if (Indices::waterEnabled) {
                pressure = fs.pressure(FluidSystem::waterPhaseIdx);
            } else {
                pressure = fs.pressure(FluidSystem::gasPhaseIdx);
            }
        }
        return pressure;
    }


    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    computePerfRateEval(const IntensiveQuantities& intQuants,
                        const std::vector<EvalWell>& mob,
                        const EvalWell& bhp,
                        const double Tw,
                        const int perf,
                        const bool allow_cf,
                        std::vector<EvalWell>& cq_s,
                        double& perf_dis_gas_rate,
                        double& perf_vap_oil_rate,
                        DeferredLogger& deferred_logger) const
    {
        const auto& fs = intQuants.fluidState();
        const EvalWell pressure = this->extendEval(getPerfCellPressure(fs));
        const EvalWell rs = this->extendEval(fs.Rs());
        const EvalWell rv = this->extendEval(fs.Rv());
        std::vector<EvalWell> b_perfcells_dense(num_components_, EvalWell{this->numWellEq_ + numEq, 0.0});
        for (unsigned phaseIdx = 0; phaseIdx < FluidSystem::numPhases; ++phaseIdx) {
            if (!FluidSystem::phaseIsActive(phaseIdx)) {
                continue;
            }
            const unsigned compIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::solventComponentIndex(phaseIdx));
            b_perfcells_dense[compIdx] =  this->extendEval(fs.invB(phaseIdx));
        }
        if constexpr (has_solvent) {
            b_perfcells_dense[contiSolventEqIdx] = this->extendEval(intQuants.solventInverseFormationVolumeFactor());
        }

        if constexpr (has_zFraction) {
            if (this->isInjector()) {
                const unsigned gasCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx);
                b_perfcells_dense[gasCompIdx] *= (1.0 - wsolvent());
                b_perfcells_dense[gasCompIdx] += wsolvent()*intQuants.zPureInvFormationVolumeFactor().value();
            }
        }

        EvalWell skin_pressure = EvalWell{this->numWellEq_ + numEq, 0.0};
        if (has_polymermw) {
            if (this->isInjector()) {
                const int pskin_index = Bhp + 1 + this->numPerfs() + perf;
                skin_pressure = this->primary_variables_evaluation_[pskin_index];
            }
        }

        // surface volume fraction of fluids within wellbore
        std::vector<EvalWell> cmix_s(this->numComponents(), EvalWell{this->numWellEq_ + numEq});
        for (int componentIdx = 0; componentIdx < this->numComponents(); ++componentIdx) {
            cmix_s[componentIdx] = this->wellSurfaceVolumeFraction(componentIdx);
        }

        computePerfRate(mob,
                        pressure,
                        bhp,
                        rs,
                        rv,
                        b_perfcells_dense,
                        Tw,
                        perf,
                        allow_cf,
                        skin_pressure,
                        cmix_s,
                        cq_s,
                        perf_dis_gas_rate,
                        perf_vap_oil_rate,
                        deferred_logger);
    }

    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    computePerfRateScalar(const IntensiveQuantities& intQuants,
                          const std::vector<Scalar>& mob,
                          const Scalar& bhp,
                          const double Tw,
                          const int perf,
                          const bool allow_cf,
                          std::vector<Scalar>& cq_s,
                          DeferredLogger& deferred_logger) const
    {
        const auto& fs = intQuants.fluidState();
        const Scalar pressure = getPerfCellPressure(fs).value();
        const Scalar rs = fs.Rs().value();
        const Scalar rv = fs.Rv().value();
        std::vector<Scalar> b_perfcells_dense(num_components_, 0.0);
        for (unsigned phaseIdx = 0; phaseIdx < FluidSystem::numPhases; ++phaseIdx) {
            if (!FluidSystem::phaseIsActive(phaseIdx)) {
                continue;
            }
            const unsigned compIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::solventComponentIndex(phaseIdx));
            b_perfcells_dense[compIdx] =  fs.invB(phaseIdx).value();
        }
        if constexpr (has_solvent) {
            b_perfcells_dense[contiSolventEqIdx] = intQuants.solventInverseFormationVolumeFactor().value();
        }

        if constexpr (has_zFraction) {
            if (this->isInjector()) {
                const unsigned gasCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx);
                b_perfcells_dense[gasCompIdx] *= (1.0 - wsolvent());
                b_perfcells_dense[gasCompIdx] += wsolvent()*intQuants.zPureInvFormationVolumeFactor().value();
            }
        }

        Scalar skin_pressure =0.0;
        if (has_polymermw) {
            if (this->isInjector()) {
                const int pskin_index = Bhp + 1 + this->numPerfs() + perf;
                skin_pressure = getValue(this->primary_variables_evaluation_[pskin_index]);
            }
        }

        Scalar perf_dis_gas_rate = 0.0;
        Scalar perf_vap_oil_rate = 0.0;

        // surface volume fraction of fluids within wellbore
        std::vector<Scalar> cmix_s(this->numComponents(), 0.0);
        for (int componentIdx = 0; componentIdx < this->numComponents(); ++componentIdx) {
            cmix_s[componentIdx] = getValue(this->wellSurfaceVolumeFraction(componentIdx));
        }

        computePerfRate(mob,
                        pressure,
                        bhp,
                        rs,
                        rv,
                        b_perfcells_dense,
                        Tw,
                        perf,
                        allow_cf,
                        skin_pressure,
                        cmix_s,
                        cq_s,
                        perf_dis_gas_rate,
                        perf_vap_oil_rate,
                        deferred_logger);
    }

    template<typename TypeTag>
    template<class Value>
    void
    StandardWell<TypeTag>::
    computePerfRate(const std::vector<Value>& mob,
                    const Value& pressure,
                    const Value& bhp,
                    const Value& rs,
                    const Value& rv,
                    std::vector<Value>& b_perfcells_dense,
                    const double Tw,
                    const int perf,
                    const bool allow_cf,
                    const Value& skin_pressure,
                    const std::vector<Value>& cmix_s,
                    std::vector<Value>& cq_s,
                    double& perf_dis_gas_rate,
                    double& perf_vap_oil_rate,
                    DeferredLogger& deferred_logger) const
    {
        // Pressure drawdown (also used to determine direction of flow)
        const Value well_pressure = bhp + this->perf_pressure_diffs_[perf];
        Value drawdown = pressure - well_pressure;
        if (this->isInjector()) {
            drawdown += skin_pressure;
        }

        // producing perforations
        if ( drawdown > 0 )  {
            //Do nothing if crossflow is not allowed
            if (!allow_cf && this->isInjector()) {
                return;
            }

            // compute component volumetric rates at standard conditions
            for (int componentIdx = 0; componentIdx < this->numComponents(); ++componentIdx) {
                const Value cq_p = - Tw * (mob[componentIdx] * drawdown);
                cq_s[componentIdx] = b_perfcells_dense[componentIdx] * cq_p;
            }

            if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx) && FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
                const unsigned oilCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::oilCompIdx);
                const unsigned gasCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx);
                const Value cq_sOil = cq_s[oilCompIdx];
                const Value cq_sGas = cq_s[gasCompIdx];
                const Value dis_gas = rs * cq_sOil;
                const Value vap_oil = rv * cq_sGas;

                cq_s[gasCompIdx] += dis_gas;
                cq_s[oilCompIdx] += vap_oil;

                // recording the perforation solution gas rate and solution oil rates
                if (this->isProducer()) {
                    perf_dis_gas_rate = getValue(dis_gas);
                    perf_vap_oil_rate = getValue(vap_oil);
                }
            }

        } else {
            //Do nothing if crossflow is not allowed
            if (!allow_cf && this->isProducer()) {
                return;
            }

            // Using total mobilities
            Value total_mob_dense = mob[0];
            for (int componentIdx = 1; componentIdx < this->numComponents(); ++componentIdx) {
                total_mob_dense += mob[componentIdx];
            }

            // injection perforations total volume rates
            const Value cqt_i = - Tw * (total_mob_dense * drawdown);

            // compute volume ratio between connection at standard conditions
            Value volumeRatio = bhp * 0.0; // initialize it with the correct type
;
            if (FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx)) {
                const unsigned waterCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::waterCompIdx);
                volumeRatio += cmix_s[waterCompIdx] / b_perfcells_dense[waterCompIdx];
            }

            if constexpr (Indices::enableSolvent) {
                volumeRatio += cmix_s[Indices::contiSolventEqIdx] / b_perfcells_dense[Indices::contiSolventEqIdx];
            }

            if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx) && FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
                const unsigned oilCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::oilCompIdx);
                const unsigned gasCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx);
                // Incorporate RS/RV factors if both oil and gas active
                const Value d = 1.0 - rv * rs;

                if (getValue(d) == 0.0) {
                    OPM_DEFLOG_THROW(NumericalIssue, "Zero d value obtained for well " << this->name() << " during flux calcuation"
                                                  << " with rs " << rs << " and rv " << rv, deferred_logger);
                }

                const Value tmp_oil = (cmix_s[oilCompIdx] - rv * cmix_s[gasCompIdx]) / d;
                volumeRatio += tmp_oil / b_perfcells_dense[oilCompIdx];

                const Value tmp_gas = (cmix_s[gasCompIdx] - rs * cmix_s[oilCompIdx]) / d;
                volumeRatio += tmp_gas / b_perfcells_dense[gasCompIdx];
            }
            else {
                if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx)) {
                    const unsigned oilCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::oilCompIdx);
                    volumeRatio += cmix_s[oilCompIdx] / b_perfcells_dense[oilCompIdx];
                }
                if (FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
                    const unsigned gasCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx);
                    volumeRatio += cmix_s[gasCompIdx] / b_perfcells_dense[gasCompIdx];
                }
            }

            // injecting connections total volumerates at standard conditions
            Value cqt_is = cqt_i/volumeRatio;
            for (int componentIdx = 0; componentIdx < this->numComponents(); ++componentIdx) {
                cq_s[componentIdx] = cmix_s[componentIdx] * cqt_is;
            }

            // calculating the perforation solution gas rate and solution oil rates
            if (this->isProducer()) {
                if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx) && FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
                    const unsigned oilCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::oilCompIdx);
                    const unsigned gasCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx);
                    // TODO: the formulations here remain to be tested with cases with strong crossflow through production wells
                    // s means standard condition, r means reservoir condition
                    // q_os = q_or * b_o + rv * q_gr * b_g
                    // q_gs = q_gr * g_g + rs * q_or * b_o
                    // d = 1.0 - rs * rv
                    // q_or = 1 / (b_o * d) * (q_os - rv * q_gs)
                    // q_gr = 1 / (b_g * d) * (q_gs - rs * q_os)

                    const double d = 1.0 - getValue(rv) * getValue(rs);
                    // vaporized oil into gas
                    // rv * q_gr * b_g = rv * (q_gs - rs * q_os) / d
                    perf_vap_oil_rate = getValue(rv) * (getValue(cq_s[gasCompIdx]) - getValue(rs) * getValue(cq_s[oilCompIdx])) / d;
                    // dissolved of gas in oil
                    // rs * q_or * b_o = rs * (q_os - rv * q_gs) / d
                    perf_dis_gas_rate = getValue(rs) * (getValue(cq_s[oilCompIdx]) - getValue(rv) * getValue(cq_s[gasCompIdx])) / d;
                }
            }
        }
    }


    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    assembleWellEqWithoutIteration(const Simulator& ebosSimulator,
                                   const double dt,
                                   const Well::InjectionControls& /*inj_controls*/,
                                   const Well::ProductionControls& /*prod_controls*/,
                                   WellState& well_state,
                                   const GroupState& group_state,
                                   DeferredLogger& deferred_logger)
    {
        // TODO: only_wells should be put back to save some computation
        // for example, the matrices B C does not need to update if only_wells
        if (!this->isOperable() && !this->wellIsStopped()) return;

        // clear all entries
        this->duneB_ = 0.0;
        this->duneC_ = 0.0;
        this->invDuneD_ = 0.0;
        this->resWell_ = 0.0;

        assembleWellEqWithoutIterationImpl(ebosSimulator, dt, well_state, group_state, deferred_logger);
    }




    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    assembleWellEqWithoutIterationImpl(const Simulator& ebosSimulator,
                                       const double dt,
                                       WellState& well_state,
                                       const GroupState& group_state,
                                       DeferredLogger& deferred_logger)
    {

        // TODO: it probably can be static member for StandardWell
        const double volume = 0.002831684659200; // 0.1 cu ft;

        // the solution gas rate and solution oil rate needs to be reset to be zero for well_state.
        well_state.wellVaporizedOilRates(index_of_well_) = 0.;
        well_state.wellDissolvedGasRates(index_of_well_) = 0.;

        const int np = number_of_phases_;

        std::vector<RateVector> connectionRates = connectionRates_; // Copy to get right size.
        auto& perf_data = well_state.perfData(this->index_of_well_);
        auto& perf_rates = perf_data.phase_rates;
        for (int perf = 0; perf < number_of_perforations_; ++perf) {
            // Calculate perforation quantities.
            std::vector<EvalWell> cq_s(num_components_, {this->numWellEq_ + numEq, 0.0});
            EvalWell water_flux_s{this->numWellEq_ + numEq, 0.0};
            EvalWell cq_s_zfrac_effective{this->numWellEq_ + numEq, 0.0};
            calculateSinglePerf(ebosSimulator, perf, well_state, connectionRates, cq_s, water_flux_s, cq_s_zfrac_effective, deferred_logger);

            // Equation assembly for this perforation.
            if constexpr (has_polymer && Base::has_polymermw) {
                if (this->isInjector()) {
                    handleInjectivityEquations(ebosSimulator, well_state, perf, water_flux_s, deferred_logger);
                }
            }
            const int cell_idx = well_cells_[perf];
            for (int componentIdx = 0; componentIdx < num_components_; ++componentIdx) {
                // the cq_s entering mass balance equations need to consider the efficiency factors.
                const EvalWell cq_s_effective = cq_s[componentIdx] * well_efficiency_factor_;

                connectionRates[perf][componentIdx] = Base::restrictEval(cq_s_effective);

                // subtract sum of phase fluxes in the well equations.
                this->resWell_[0][componentIdx] += cq_s_effective.value();

                // assemble the jacobians
                for (int pvIdx = 0; pvIdx < this->numWellEq_; ++pvIdx) {
                    // also need to consider the efficiency factor when manipulating the jacobians.
                    this->duneC_[0][cell_idx][pvIdx][componentIdx] -= cq_s_effective.derivative(pvIdx+numEq); // intput in transformed matrix
                    this->invDuneD_[0][0][componentIdx][pvIdx] += cq_s_effective.derivative(pvIdx+numEq);
                }

                for (int pvIdx = 0; pvIdx < numEq; ++pvIdx) {
                    this->duneB_[0][cell_idx][componentIdx][pvIdx] += cq_s_effective.derivative(pvIdx);
                }

                // Store the perforation phase flux for later usage.
                if (has_solvent && componentIdx == contiSolventEqIdx) {
                    auto& perf_rate_solvent = perf_data.solvent_rates;
                    perf_rate_solvent[perf] = cq_s[componentIdx].value();
                } else {
                    perf_rates[perf*np + ebosCompIdxToFlowCompIdx(componentIdx)] = cq_s[componentIdx].value();
                }
            }

            if constexpr (has_zFraction) {
                for (int pvIdx = 0; pvIdx < this->numWellEq_; ++pvIdx) {
                    this->duneC_[0][cell_idx][pvIdx][contiZfracEqIdx] -= cq_s_zfrac_effective.derivative(pvIdx+numEq);
                }
            }
        }
        // Update the connection
        connectionRates_ = connectionRates;

        // accumulate resWell_ and invDuneD_ in parallel to get effects of all perforations (might be distributed)
        wellhelpers::sumDistributedWellEntries(this->invDuneD_[0][0], this->resWell_[0],
                                               this->parallel_well_info_.communication());
        // add vol * dF/dt + Q to the well equations;
        for (int componentIdx = 0; componentIdx < numWellConservationEq; ++componentIdx) {
            // TODO: following the development in MSW, we need to convert the volume of the wellbore to be surface volume
            // since all the rates are under surface condition
            EvalWell resWell_loc(this->numWellEq_ + numEq, 0.0);
            if (FluidSystem::numActivePhases() > 1) {
                assert(dt > 0);
                resWell_loc += (this->wellSurfaceVolumeFraction(componentIdx) - this->F0_[componentIdx]) * volume / dt;
            }
            resWell_loc -= this->getQs(componentIdx) * well_efficiency_factor_;
            for (int pvIdx = 0; pvIdx < this->numWellEq_; ++pvIdx) {
                this->invDuneD_[0][0][componentIdx][pvIdx] += resWell_loc.derivative(pvIdx+numEq);
            }
            this->resWell_[0][componentIdx] += resWell_loc.value();
        }

        const auto& summaryState = ebosSimulator.vanguard().summaryState();
        const Schedule& schedule = ebosSimulator.vanguard().schedule();
        this->assembleControlEq(well_state, group_state, schedule, summaryState, deferred_logger);


        // do the local inversion of D.
        try {
            Dune::ISTLUtility::invertMatrix(this->invDuneD_[0][0]);
        } catch( ... ) {
            OPM_DEFLOG_THROW(NumericalIssue,"Error when inverting local well equations for well " + name(), deferred_logger);
        }
    }




    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    calculateSinglePerf(const Simulator& ebosSimulator,
                        const int perf,
                        WellState& well_state,
                        std::vector<RateVector>& connectionRates,
                        std::vector<EvalWell>& cq_s,
                        EvalWell& water_flux_s,
                        EvalWell& cq_s_zfrac_effective,
                        DeferredLogger& deferred_logger) const
    {
        const bool allow_cf = getAllowCrossFlow() || openCrossFlowAvoidSingularity(ebosSimulator);
        const EvalWell& bhp = this->getBhp();
        const int cell_idx = well_cells_[perf];
        const auto& intQuants = *(ebosSimulator.model().cachedIntensiveQuantities(cell_idx, /*timeIdx=*/ 0));
        std::vector<EvalWell> mob(num_components_, {this->numWellEq_ + numEq, 0.});
        getMobilityEval(ebosSimulator, perf, mob, deferred_logger);

        double perf_dis_gas_rate = 0.;
        double perf_vap_oil_rate = 0.;
        double trans_mult = ebosSimulator.problem().template rockCompTransMultiplier<double>(intQuants,  cell_idx);
        const double Tw = well_index_[perf] * trans_mult;
        computePerfRateEval(intQuants, mob, bhp, Tw, perf, allow_cf,
                            cq_s, perf_dis_gas_rate, perf_vap_oil_rate, deferred_logger);

        auto& perf_data = well_state.perfData(this->index_of_well_);
        if constexpr (has_polymer && Base::has_polymermw) {
            if (this->isInjector()) {
                // Store the original water flux computed from the reservoir quantities.
                // It will be required to assemble the injectivity equations.
                const unsigned water_comp_idx = Indices::canonicalToActiveComponentIndex(FluidSystem::waterCompIdx);
                water_flux_s = cq_s[water_comp_idx];
                // Modify the water flux for the rest of this function to depend directly on the
                // local water velocity primary variable.
                handleInjectivityRate(ebosSimulator, perf, cq_s);
            }
        }

        // updating the solution gas rate and solution oil rate
        if (this->isProducer()) {
            well_state.wellDissolvedGasRates(index_of_well_) += perf_dis_gas_rate;
            well_state.wellVaporizedOilRates(index_of_well_) += perf_vap_oil_rate;
        }

        if constexpr (has_energy) {
            connectionRates[perf][contiEnergyEqIdx] = 0.0;
        }

        if constexpr (has_energy) {

            auto fs = intQuants.fluidState();
            for (unsigned phaseIdx = 0; phaseIdx < FluidSystem::numPhases; ++phaseIdx) {
                if (!FluidSystem::phaseIsActive(phaseIdx)) {
                    continue;
                }

                const unsigned activeCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::solventComponentIndex(phaseIdx));
                // convert to reservoar conditions
                EvalWell cq_r_thermal(this->numWellEq_ + numEq, 0.);
                if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx) && FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {

                    if(FluidSystem::waterPhaseIdx == phaseIdx)
                        cq_r_thermal = cq_s[activeCompIdx] / this->extendEval(fs.invB(phaseIdx));

                    // remove dissolved gas and vapporized oil
                    const unsigned oilCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::oilCompIdx);
                    const unsigned gasCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx);
                    // q_os = q_or * b_o + rv * q_gr * b_g
                    // q_gs = q_gr * g_g + rs * q_or * b_o
                    // d = 1.0 - rs * rv
                    const EvalWell d = this->extendEval(1.0 - fs.Rv() * fs.Rs());
                    // q_gr = 1 / (b_g * d) * (q_gs - rs * q_os)
                    if(FluidSystem::gasPhaseIdx == phaseIdx)
                        cq_r_thermal = (cq_s[gasCompIdx] - this->extendEval(fs.Rs()) * cq_s[oilCompIdx]) / (d * this->extendEval(fs.invB(phaseIdx)) );
                    // q_or = 1 / (b_o * d) * (q_os - rv * q_gs)
                    if(FluidSystem::oilPhaseIdx == phaseIdx)
                        cq_r_thermal = (cq_s[oilCompIdx] - this->extendEval(fs.Rv()) * cq_s[gasCompIdx]) / (d * this->extendEval(fs.invB(phaseIdx)) );

                } else {
                    cq_r_thermal = cq_s[activeCompIdx] / this->extendEval(fs.invB(phaseIdx));
                }

                // change temperature for injecting fluids
                if (this->isInjector() && cq_s[activeCompIdx] > 0.0){
                    // only handles single phase injection now
                    assert(this->well_ecl_.injectorType() != InjectorType::MULTI);
                    fs.setTemperature(this->well_ecl_.temperature());
                    typedef typename std::decay<decltype(fs)>::type::Scalar FsScalar;
                    typename FluidSystem::template ParameterCache<FsScalar> paramCache;
                    const unsigned pvtRegionIdx = intQuants.pvtRegionIndex();
                    paramCache.setRegionIndex(pvtRegionIdx);
                    paramCache.setMaxOilSat(ebosSimulator.problem().maxOilSaturation(cell_idx));
                    paramCache.updatePhase(fs, phaseIdx);

                    const auto& rho = FluidSystem::density(fs, paramCache, phaseIdx);
                    fs.setDensity(phaseIdx, rho);
                    const auto& h = FluidSystem::enthalpy(fs, paramCache, phaseIdx);
                    fs.setEnthalpy(phaseIdx, h);
                }
                // compute the thermal flux
                cq_r_thermal *= this->extendEval(fs.enthalpy(phaseIdx)) * this->extendEval(fs.density(phaseIdx));
                connectionRates[perf][contiEnergyEqIdx] += Base::restrictEval(cq_r_thermal);
            }
        }

        if constexpr (has_polymer) {
            // TODO: the application of well efficiency factor has not been tested with an example yet
            const unsigned waterCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::waterCompIdx);
            EvalWell cq_s_poly = cq_s[waterCompIdx];
            if (this->isInjector()) {
                cq_s_poly *= wpolymer();
            } else {
                cq_s_poly *= this->extendEval(intQuants.polymerConcentration() * intQuants.polymerViscosityCorrection());
            }
            // Note. Efficiency factor is handled in the output layer
            auto& perf_rate_polymer = perf_data.polymer_rates;
            perf_rate_polymer[perf] = cq_s_poly.value();

            cq_s_poly *= well_efficiency_factor_;
            connectionRates[perf][contiPolymerEqIdx] = Base::restrictEval(cq_s_poly);

            if constexpr (Base::has_polymermw) {
                updateConnectionRatePolyMW(cq_s_poly, intQuants, well_state, perf, connectionRates, deferred_logger);
            }
        }

        if constexpr (has_foam) {
            // TODO: the application of well efficiency factor has not been tested with an example yet
            const unsigned gasCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx);
            EvalWell cq_s_foam = cq_s[gasCompIdx] * well_efficiency_factor_;
            if (this->isInjector()) {
                cq_s_foam *= wfoam();
            } else {
                cq_s_foam *= this->extendEval(intQuants.foamConcentration());
            }
            connectionRates[perf][contiFoamEqIdx] = Base::restrictEval(cq_s_foam);
        }

        if constexpr (has_zFraction) {
            // TODO: the application of well efficiency factor has not been tested with an example yet
            const unsigned gasCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx);
            cq_s_zfrac_effective = cq_s[gasCompIdx];
            if (this->isInjector()) {
                cq_s_zfrac_effective *= wsolvent();
            } else if (cq_s_zfrac_effective.value() != 0.0) {
                const double dis_gas_frac = perf_dis_gas_rate / cq_s_zfrac_effective.value();
                cq_s_zfrac_effective *= this->extendEval(dis_gas_frac*intQuants.xVolume() + (1.0-dis_gas_frac)*intQuants.yVolume());
            }
            auto& perf_rate_solvent = perf_data.solvent_rates;
            perf_rate_solvent[perf] = cq_s_zfrac_effective.value();

            cq_s_zfrac_effective *= well_efficiency_factor_;
            connectionRates[perf][contiZfracEqIdx] = Base::restrictEval(cq_s_zfrac_effective);
        }

        if constexpr (has_brine) {
            // TODO: the application of well efficiency factor has not been tested with an example yet
            const unsigned waterCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::waterCompIdx);
            EvalWell cq_s_sm = cq_s[waterCompIdx];
            if (this->isInjector()) {
                cq_s_sm *= wsalt();
            } else {
                cq_s_sm *= this->extendEval(intQuants.fluidState().saltConcentration());
            }
            // Note. Efficiency factor is handled in the output layer
            auto& perf_rate_brine = perf_data.brine_rates;
            perf_rate_brine[perf] = cq_s_sm.value();

            cq_s_sm *= well_efficiency_factor_;
            connectionRates[perf][contiBrineEqIdx] = Base::restrictEval(cq_s_sm);
        }

        // Store the perforation pressure for later usage.
        perf_data.pressure[perf] = well_state.bhp(this->index_of_well_) + this->perf_pressure_diffs_[perf];
    }




    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    getMobilityEval(const Simulator& ebosSimulator,
                    const int perf,
                    std::vector<EvalWell>& mob,
                    DeferredLogger& deferred_logger) const
    {
        const int cell_idx = well_cells_[perf];
        assert (int(mob.size()) == num_components_);
        const auto& intQuants = *(ebosSimulator.model().cachedIntensiveQuantities(cell_idx, /*timeIdx=*/0));
        const auto& materialLawManager = ebosSimulator.problem().materialLawManager();

        // either use mobility of the perforation cell or calcualte its own
        // based on passing the saturation table index
        const int satid = saturation_table_number_[perf] - 1;
        const int satid_elem = materialLawManager->satnumRegionIdx(cell_idx);
        if( satid == satid_elem ) { // the same saturation number is used. i.e. just use the mobilty from the cell

            for (unsigned phaseIdx = 0; phaseIdx < FluidSystem::numPhases; ++phaseIdx) {
                if (!FluidSystem::phaseIsActive(phaseIdx)) {
                    continue;
                }

                const unsigned activeCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::solventComponentIndex(phaseIdx));
                mob[activeCompIdx] = this->extendEval(intQuants.mobility(phaseIdx));
            }
            if (has_solvent) {
                mob[contiSolventEqIdx] = this->extendEval(intQuants.solventMobility());
            }
        } else {

            const auto& paramsCell = materialLawManager->connectionMaterialLawParams(satid, cell_idx);
            Eval relativePerms[3] = { 0.0, 0.0, 0.0 };
            MaterialLaw::relativePermeabilities(relativePerms, paramsCell, intQuants.fluidState());

            // reset the satnumvalue back to original
            materialLawManager->connectionMaterialLawParams(satid_elem, cell_idx);

            // compute the mobility
            for (unsigned phaseIdx = 0; phaseIdx < FluidSystem::numPhases; ++phaseIdx) {
                if (!FluidSystem::phaseIsActive(phaseIdx)) {
                    continue;
                }

                const unsigned activeCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::solventComponentIndex(phaseIdx));
                mob[activeCompIdx] = this->extendEval(relativePerms[phaseIdx] / intQuants.fluidState().viscosity(phaseIdx));
            }

            // this may not work if viscosity and relperms has been modified?
            if constexpr (has_solvent) {
                OPM_DEFLOG_THROW(std::runtime_error, "individual mobility for wells does not work in combination with solvent", deferred_logger);
            }
        }

        // modify the water mobility if polymer is present
        if constexpr (has_polymer) {
            if (!FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx)) {
                OPM_DEFLOG_THROW(std::runtime_error, "Water is required when polymer is active", deferred_logger);
            }

            // for the cases related to polymer molecular weight, we assume fully mixing
            // as a result, the polymer and water share the same viscosity
            if constexpr (!Base::has_polymermw) {
                updateWaterMobilityWithPolymer(ebosSimulator, perf, mob, deferred_logger);
            }
        }
    }

    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    getMobilityScalar(const Simulator& ebosSimulator,
                      const int perf,
                      std::vector<Scalar>& mob,
                      DeferredLogger& deferred_logger) const
    {
        const int cell_idx = well_cells_[perf];
        assert (int(mob.size()) == num_components_);
        const auto& intQuants = *(ebosSimulator.model().cachedIntensiveQuantities(cell_idx, /*timeIdx=*/0));
        const auto& materialLawManager = ebosSimulator.problem().materialLawManager();

        // either use mobility of the perforation cell or calcualte its own
        // based on passing the saturation table index
        const int satid = saturation_table_number_[perf] - 1;
        const int satid_elem = materialLawManager->satnumRegionIdx(cell_idx);
        if( satid == satid_elem ) { // the same saturation number is used. i.e. just use the mobilty from the cell

            for (unsigned phaseIdx = 0; phaseIdx < FluidSystem::numPhases; ++phaseIdx) {
                if (!FluidSystem::phaseIsActive(phaseIdx)) {
                    continue;
                }

                const unsigned activeCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::solventComponentIndex(phaseIdx));
                mob[activeCompIdx] = getValue(intQuants.mobility(phaseIdx));
            }
            if (has_solvent) {
                mob[contiSolventEqIdx] = getValue(intQuants.solventMobility());
            }
        } else {

            const auto& paramsCell = materialLawManager->connectionMaterialLawParams(satid, cell_idx);
            Eval relativePerms[3] = { 0.0, 0.0, 0.0 };
            MaterialLaw::relativePermeabilities(relativePerms, paramsCell, intQuants.fluidState());

            // reset the satnumvalue back to original
            materialLawManager->connectionMaterialLawParams(satid_elem, cell_idx);

            // compute the mobility
            for (unsigned phaseIdx = 0; phaseIdx < FluidSystem::numPhases; ++phaseIdx) {
                if (!FluidSystem::phaseIsActive(phaseIdx)) {
                    continue;
                }

                const unsigned activeCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::solventComponentIndex(phaseIdx));
                mob[activeCompIdx] = getValue(relativePerms[phaseIdx]) / getValue(intQuants.fluidState().viscosity(phaseIdx));
            }

            // this may not work if viscosity and relperms has been modified?
            if constexpr (has_solvent) {
                OPM_DEFLOG_THROW(std::runtime_error, "individual mobility for wells does not work in combination with solvent", deferred_logger);
            }
        }

        // modify the water mobility if polymer is present
        if constexpr (has_polymer) {
            if (!FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx)) {
                OPM_DEFLOG_THROW(std::runtime_error, "Water is required when polymer is active", deferred_logger);
            }

            // for the cases related to polymer molecular weight, we assume fully mixing
            // as a result, the polymer and water share the same viscosity
            if constexpr (!Base::has_polymermw) {
                std::vector<EvalWell> mob_eval(num_components_, {this->numWellEq_ + numEq, 0.});
                updateWaterMobilityWithPolymer(ebosSimulator, perf, mob_eval, deferred_logger);
                for (size_t i = 0; i < mob.size(); ++i) {
                    mob[i] = getValue(mob_eval[i]);
                }
            }
        }
    }



    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    updateWellState(const BVectorWell& dwells,
                    WellState& well_state,
                    DeferredLogger& deferred_logger) const
    {
        if (!this->isOperable() && !this->wellIsStopped()) return;

        updatePrimaryVariablesNewton(dwells, well_state);

        updateWellStateFromPrimaryVariables(well_state, deferred_logger);
        Base::calculateReservoirRates(well_state);
    }





    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    updatePrimaryVariablesNewton(const BVectorWell& dwells,
                                 const WellState& /* well_state */) const
    {
        const double dFLimit = param_.dwell_fraction_max_;
        const double dBHPLimit = param_.dbhp_max_rel_;
        this->StdWellEval::updatePrimaryVariablesNewton(dwells, dFLimit, dBHPLimit);

        updateExtraPrimaryVariables(dwells);

#ifndef NDEBUG
        for (double v : this->primary_variables_) {
            assert(isfinite(v));
        }
#endif

    }





    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    updateExtraPrimaryVariables(const BVectorWell& dwells) const
    {
        // for the water velocity and skin pressure
        if constexpr (Base::has_polymermw) {
            this->updatePrimaryVariablesPolyMW(dwells);
        }
    }





    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    updateWellStateFromPrimaryVariables(WellState& well_state, DeferredLogger& deferred_logger) const
    {
        this->StdWellEval::updateWellStateFromPrimaryVariables(well_state, deferred_logger);

        // other primary variables related to polymer injectivity study
        if constexpr (Base::has_polymermw) {
            this->updateWellStateFromPrimaryVariablesPolyMW(well_state);
        }
    }





    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    updateIPR(const Simulator& ebos_simulator, DeferredLogger& deferred_logger) const
    {
        // TODO: not handling solvent related here for now

        // TODO: it only handles the producers for now
        // the formular for the injectors are not formulated yet
        if (this->isInjector()) {
            return;
        }

        // initialize all the values to be zero to begin with
        std::fill(ipr_a_.begin(), ipr_a_.end(), 0.);
        std::fill(ipr_b_.begin(), ipr_b_.end(), 0.);

        for (int perf = 0; perf < number_of_perforations_; ++perf) {
            std::vector<EvalWell> mob(num_components_, {this->numWellEq_ + numEq, 0.0});
            // TODO: mabye we should store the mobility somewhere, so that we only need to calculate it one per iteration
            getMobilityEval(ebos_simulator, perf, mob, deferred_logger);

            const int cell_idx = well_cells_[perf];
            const auto& int_quantities = *(ebos_simulator.model().cachedIntensiveQuantities(cell_idx, /*timeIdx=*/ 0));
            const auto& fs = int_quantities.fluidState();
            // the pressure of the reservoir grid block the well connection is in
            Eval perf_pressure = getPerfCellPressure(fs);
            double p_r = perf_pressure.value();

            // calculating the b for the connection
            std::vector<double> b_perf(num_components_);
            for (size_t phase = 0; phase < FluidSystem::numPhases; ++phase) {
                if (!FluidSystem::phaseIsActive(phase)) {
                    continue;
                }
                const unsigned comp_idx = Indices::canonicalToActiveComponentIndex(FluidSystem::solventComponentIndex(phase));
                b_perf[comp_idx] = fs.invB(phase).value();
            }

            // the pressure difference between the connection and BHP
            const double h_perf = this->perf_pressure_diffs_[perf];
            const double pressure_diff = p_r - h_perf;

            // Let us add a check, since the pressure is calculated based on zero value BHP
            // it should not be negative anyway. If it is negative, we might need to re-formulate
            // to taking into consideration the crossflow here.
            if (pressure_diff <= 0.) {
                deferred_logger.warning("NON_POSITIVE_DRAWDOWN_IPR",
                                "non-positive drawdown found when updateIPR for well " + name());
            }

            // the well index associated with the connection
            const double tw_perf = well_index_[perf]*ebos_simulator.problem().template rockCompTransMultiplier<double>(int_quantities, cell_idx);

            // TODO: there might be some indices related problems here
            // phases vs components
            // ipr values for the perforation
            std::vector<double> ipr_a_perf(ipr_a_.size());
            std::vector<double> ipr_b_perf(ipr_b_.size());
            for (int p = 0; p < number_of_phases_; ++p) {
                const double tw_mob = tw_perf * mob[p].value() * b_perf[p];
                ipr_a_perf[p] += tw_mob * pressure_diff;
                ipr_b_perf[p] += tw_mob;
            }

            // we need to handle the rs and rv when both oil and gas are present
            if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx) && FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
                const unsigned oil_comp_idx = Indices::canonicalToActiveComponentIndex(FluidSystem::oilCompIdx);
                const unsigned gas_comp_idx = Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx);
                const double rs = (fs.Rs()).value();
                const double rv = (fs.Rv()).value();

                const double dis_gas_a = rs * ipr_a_perf[oil_comp_idx];
                const double vap_oil_a = rv * ipr_a_perf[gas_comp_idx];

                ipr_a_perf[gas_comp_idx] += dis_gas_a;
                ipr_a_perf[oil_comp_idx] += vap_oil_a;

                const double dis_gas_b = rs * ipr_b_perf[oil_comp_idx];
                const double vap_oil_b = rv * ipr_b_perf[gas_comp_idx];

                ipr_b_perf[gas_comp_idx] += dis_gas_b;
                ipr_b_perf[oil_comp_idx] += vap_oil_b;
            }

            for (int p = 0; p < number_of_phases_; ++p) {
                // TODO: double check the indices here
                ipr_a_[ebosCompIdxToFlowCompIdx(p)] += ipr_a_perf[p];
                ipr_b_[ebosCompIdxToFlowCompIdx(p)] += ipr_b_perf[p];
            }
        }
        this->parallel_well_info_.communication().sum(ipr_a_.data(), ipr_a_.size());
        this->parallel_well_info_.communication().sum(ipr_b_.data(), ipr_b_.size());
    }


    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    checkOperabilityUnderBHPLimitProducer(const WellState& well_state, const Simulator& ebos_simulator, DeferredLogger& deferred_logger)
    {
        const auto& summaryState = ebos_simulator.vanguard().summaryState();
        const double bhp_limit = mostStrictBhpFromBhpLimits(summaryState);
        // Crude but works: default is one atmosphere.
        // TODO: a better way to detect whether the BHP is defaulted or not
        const bool bhp_limit_not_defaulted = bhp_limit > 1.5 * unit::barsa;
        if ( bhp_limit_not_defaulted || !this->wellHasTHPConstraints(summaryState) ) {
            // if the BHP limit is not defaulted or the well does not have a THP limit
            // we need to check the BHP limit

            for (int p = 0; p < number_of_phases_; ++p) {
                const double temp = ipr_a_[p] - ipr_b_[p] * bhp_limit;
                if (temp < 0.) {
                    this->operability_status_.operable_under_only_bhp_limit = false;
                    break;
                }
            }

            // checking whether running under BHP limit will violate THP limit
            if (this->operability_status_.operable_under_only_bhp_limit && this->wellHasTHPConstraints(summaryState)) {
                // option 1: calculate well rates based on the BHP limit.
                // option 2: stick with the above IPR curve
                // we use IPR here
                std::vector<double> well_rates_bhp_limit;
                computeWellRatesWithBhp(ebos_simulator, bhp_limit, well_rates_bhp_limit, deferred_logger);

                const double thp = this->calculateThpFromBhp(well_state, well_rates_bhp_limit, bhp_limit, deferred_logger);
                const double thp_limit = this->getTHPConstraint(summaryState);

                if (thp < thp_limit) {
                    this->operability_status_.obey_thp_limit_under_bhp_limit = false;
                }
            }
        } else {
            // defaulted BHP and there is a THP constraint
            // default BHP limit is about 1 atm.
            // when applied the hydrostatic pressure correction dp,
            // most likely we get a negative value (bhp + dp)to search in the VFP table,
            // which is not desirable.
            // we assume we can operate under defaulted BHP limit and will violate the THP limit
            // when operating under defaulted BHP limit.
            this->operability_status_.operable_under_only_bhp_limit = true;
            this->operability_status_.obey_thp_limit_under_bhp_limit = false;
        }
    }





    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    checkOperabilityUnderTHPLimitProducer(const Simulator& ebos_simulator, const WellState& well_state, DeferredLogger& deferred_logger)
    {
        const auto& summaryState = ebos_simulator.vanguard().summaryState();
        const auto obtain_bhp = computeBhpAtThpLimitProd(well_state, ebos_simulator, summaryState, deferred_logger);

        if (obtain_bhp) {
            this->operability_status_.can_obtain_bhp_with_thp_limit = true;

            const double  bhp_limit = mostStrictBhpFromBhpLimits(summaryState);
            this->operability_status_.obey_bhp_limit_with_thp_limit = (*obtain_bhp >= bhp_limit);

            const double thp_limit = this->getTHPConstraint(summaryState);
            if (*obtain_bhp < thp_limit) {
                const std::string msg = " obtained bhp " + std::to_string(unit::convert::to(*obtain_bhp, unit::barsa))
                                        + " bars is SMALLER than thp limit "
                                        + std::to_string(unit::convert::to(thp_limit, unit::barsa))
                                        + " bars as a producer for well " + name();
                deferred_logger.debug(msg);
            }
        } else {
            this->operability_status_.can_obtain_bhp_with_thp_limit = false;
            this->operability_status_.obey_bhp_limit_with_thp_limit = false;
            if (!this->wellIsStopped()) {
                const double thp_limit = this->getTHPConstraint(summaryState);
                deferred_logger.debug(" could not find bhp value at thp limit "
                                      + std::to_string(unit::convert::to(thp_limit, unit::barsa))
                                      + " bar for well " + name() + ", the well might need to be closed ");
            }
        }
    }





    template<typename TypeTag>
    bool
    StandardWell<TypeTag>::
    allDrawDownWrongDirection(const Simulator& ebos_simulator) const
    {
        bool all_drawdown_wrong_direction = true;

        for (int perf = 0; perf < number_of_perforations_; ++perf) {
            const int cell_idx = well_cells_[perf];
            const auto& intQuants = *(ebos_simulator.model().cachedIntensiveQuantities(cell_idx, /*timeIdx=*/0));
            const auto& fs = intQuants.fluidState();

            const double pressure = (fs.pressure(FluidSystem::oilPhaseIdx)).value();
            const double bhp = this->getBhp().value();

            // Pressure drawdown (also used to determine direction of flow)
            const double well_pressure = bhp + this->perf_pressure_diffs_[perf];
            const double drawdown = pressure - well_pressure;

            // for now, if there is one perforation can produce/inject in the correct
            // direction, we consider this well can still produce/inject.
            // TODO: it can be more complicated than this to cause wrong-signed rates
            if ( (drawdown < 0. && this->isInjector()) ||
                 (drawdown > 0. && this->isProducer()) )  {
                all_drawdown_wrong_direction = false;
                break;
            }
        }

        const auto& comm = this->parallel_well_info_.communication();
        if (comm.size() > 1)
        {
            all_drawdown_wrong_direction =
                (comm.min(all_drawdown_wrong_direction ? 1 : 0) == 1);
        }

        return all_drawdown_wrong_direction;
    }




    template<typename TypeTag>
    bool
    StandardWell<TypeTag>::
    canProduceInjectWithCurrentBhp(const Simulator& ebos_simulator,
                                   const WellState& well_state,
                                   DeferredLogger& deferred_logger)
    {
        const double bhp = well_state.bhp(index_of_well_);
        std::vector<double> well_rates;
        computeWellRatesWithBhp(ebos_simulator, bhp, well_rates, deferred_logger);

        const double sign = (this->isProducer()) ? -1. : 1.;
        const double threshold = sign * std::numeric_limits<double>::min();

        bool can_produce_inject = false;
        for (const auto value : well_rates) {
            if (this->isProducer() && value < threshold) {
                can_produce_inject = true;
                break;
            } else if (this->isInjector() && value > threshold) {
                can_produce_inject = true;
                break;
            }
        }

        if (!can_produce_inject) {
            deferred_logger.debug(" well " + name() + " CANNOT produce or inejct ");
        }

        return can_produce_inject;
    }





    template<typename TypeTag>
    bool
    StandardWell<TypeTag>::
    openCrossFlowAvoidSingularity(const Simulator& ebos_simulator) const
    {
        return !getAllowCrossFlow() && allDrawDownWrongDirection(ebos_simulator);
    }




    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    computePropertiesForWellConnectionPressures(const Simulator& ebosSimulator,
                                                const WellState& well_state,
                                                std::vector<double>& b_perf,
                                                std::vector<double>& rsmax_perf,
                                                std::vector<double>& rvmax_perf,
                                                std::vector<double>& surf_dens_perf) const
    {
        const int nperf = number_of_perforations_;
        const PhaseUsage& pu = phaseUsage();
        b_perf.resize(nperf * num_components_);
        surf_dens_perf.resize(nperf * num_components_);
        const int w = index_of_well_;

        const bool waterPresent = FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx);
        const bool oilPresent = FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx);
        const bool gasPresent = FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx);

        //rs and rv are only used if both oil and gas is present
        if (oilPresent && gasPresent) {
            rsmax_perf.resize(nperf);
            rvmax_perf.resize(nperf);
        }

        // Compute the average pressure in each well block
        const auto& perf_press = well_state.perfData(w).pressure;
        auto p_above =  this->parallel_well_info_.communicateAboveValues(well_state.bhp(w),
                                                                         perf_press.data(),
                                                                         nperf);

        for (int perf = 0; perf < nperf; ++perf) {
            const int cell_idx = well_cells_[perf];
            const auto& intQuants = *(ebosSimulator.model().cachedIntensiveQuantities(cell_idx, /*timeIdx=*/0));
            const auto& fs = intQuants.fluidState();

            // TODO: this is another place to show why WellState need to be a vector of WellState.
            // TODO: to check why should be perf - 1
            const double p_avg = (perf_press[perf] + p_above[perf])/2;
            const double temperature = fs.temperature(FluidSystem::oilPhaseIdx).value();
            const double saltConcentration = fs.saltConcentration().value();

            if (waterPresent) {
                const unsigned waterCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::waterCompIdx);
                b_perf[ waterCompIdx + perf * num_components_] =
                FluidSystem::waterPvt().inverseFormationVolumeFactor(fs.pvtRegionIndex(), temperature, p_avg, saltConcentration);
            }

            if (gasPresent) {
                const unsigned gasCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx);
                const int gaspos = gasCompIdx + perf * num_components_;

                if (oilPresent) {
                    const double oilrate = std::abs(well_state.wellRates(w)[pu.phase_pos[Oil]]); //in order to handle negative rates in producers
                    rvmax_perf[perf] = FluidSystem::gasPvt().saturatedOilVaporizationFactor(fs.pvtRegionIndex(), temperature, p_avg);
                    if (oilrate > 0) {
                        const double gasrate = std::abs(well_state.wellRates(w)[pu.phase_pos[Gas]]) - (has_solvent ? well_state.solventWellRate(w) : 0.0);
                        double rv = 0.0;
                        if (gasrate > 0) {
                            rv = oilrate / gasrate;
                        }
                        rv = std::min(rv, rvmax_perf[perf]);

                        b_perf[gaspos] = FluidSystem::gasPvt().inverseFormationVolumeFactor(fs.pvtRegionIndex(), temperature, p_avg, rv);
                    }
                    else {
                        b_perf[gaspos] = FluidSystem::gasPvt().saturatedInverseFormationVolumeFactor(fs.pvtRegionIndex(), temperature, p_avg);
                    }

                } else {
                    b_perf[gaspos] = FluidSystem::gasPvt().saturatedInverseFormationVolumeFactor(fs.pvtRegionIndex(), temperature, p_avg);
                }
            }

            if (oilPresent) {
                const unsigned oilCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::oilCompIdx);
                const int oilpos = oilCompIdx + perf * num_components_;
                if (gasPresent) {
                    rsmax_perf[perf] = FluidSystem::oilPvt().saturatedGasDissolutionFactor(fs.pvtRegionIndex(), temperature, p_avg);
                    const double gasrate = std::abs(well_state.wellRates(w)[pu.phase_pos[Gas]]) - (has_solvent ? well_state.solventWellRate(w) : 0.0);
                    if (gasrate > 0) {
                        const double oilrate = std::abs(well_state.wellRates(w)[pu.phase_pos[Oil]]);
                        double rs = 0.0;
                        if (oilrate > 0) {
                            rs = gasrate / oilrate;
                        }
                        rs = std::min(rs, rsmax_perf[perf]);
                        b_perf[oilpos] = FluidSystem::oilPvt().inverseFormationVolumeFactor(fs.pvtRegionIndex(), temperature, p_avg, rs);
                    } else {
                        b_perf[oilpos] = FluidSystem::oilPvt().saturatedInverseFormationVolumeFactor(fs.pvtRegionIndex(), temperature, p_avg);
                    }
                } else {
                    b_perf[oilpos] = FluidSystem::oilPvt().saturatedInverseFormationVolumeFactor(fs.pvtRegionIndex(), temperature, p_avg);
                }
            }

            // Surface density.
            for (unsigned phaseIdx = 0; phaseIdx < FluidSystem::numPhases; ++phaseIdx) {
                if (!FluidSystem::phaseIsActive(phaseIdx)) {
                    continue;
                }

                const unsigned compIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::solventComponentIndex(phaseIdx));
                surf_dens_perf[num_components_ * perf  + compIdx] = FluidSystem::referenceDensity( phaseIdx, fs.pvtRegionIndex() );
            }

            // We use cell values for solvent injector
            if constexpr (has_solvent) {
                b_perf[num_components_ * perf + contiSolventEqIdx] = intQuants.solventInverseFormationVolumeFactor().value();
                surf_dens_perf[num_components_ * perf + contiSolventEqIdx] = intQuants.solventRefDensity();
            }
        }
    }





    template<typename TypeTag>
    ConvergenceReport
    StandardWell<TypeTag>::
    getWellConvergence(const WellState& well_state,
                       const std::vector<double>& B_avg,
                       DeferredLogger& deferred_logger,
                       const bool /*relax_tolerance*/) const
    {
        // the following implementation assume that the polymer is always after the w-o-g phases
        // For the polymer, energy and foam cases, there is one more mass balance equations of reservoir than wells
        assert((int(B_avg.size()) == num_components_) || has_polymer || has_energy || has_foam || has_brine || has_zFraction);

        const double tol_wells = param_.tolerance_wells_;
        const double maxResidualAllowed = param_.max_residual_allowed_;

        std::vector<double> res;
        ConvergenceReport report = this->StdWellEval::getWellConvergence(well_state,
                                                                         B_avg,
                                                                         tol_wells,
                                                                         maxResidualAllowed,
                                                                         res,
                                                                         deferred_logger);
        checkConvergenceExtraEqs(res, report);

        return report;
    }





    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    updateProductivityIndex(const Simulator& ebosSimulator,
                            const WellProdIndexCalculator& wellPICalc,
                            WellState& well_state,
                            DeferredLogger& deferred_logger) const
    {
        auto fluidState = [&ebosSimulator, this](const int perf)
        {
            const auto cell_idx = this->well_cells_[perf];
            return ebosSimulator.model()
               .cachedIntensiveQuantities(cell_idx, /*timeIdx=*/ 0)->fluidState();
        };

        const int np = this->number_of_phases_;
        auto setToZero = [np](double* x) -> void
        {
            std::fill_n(x, np, 0.0);
        };

        auto addVector = [np](const double* src, double* dest) -> void
        {
            std::transform(src, src + np, dest, dest, std::plus<>{});
        };

        auto& perf_data = well_state.perfData(this->index_of_well_);
        auto* wellPI = well_state.productivityIndex(this->index_of_well_).data();
        auto* connPI = perf_data.prod_index.data();

        setToZero(wellPI);

        const auto preferred_phase = this->well_ecl_.getPreferredPhase();
        auto subsetPerfID = 0;

        for (const auto& perf : *this->perf_data_) {
            auto allPerfID = perf.ecl_index;

            auto connPICalc = [&wellPICalc, allPerfID](const double mobility) -> double
            {
                return wellPICalc.connectionProdIndStandard(allPerfID, mobility);
            };

            std::vector<EvalWell> mob(num_components_, {this->numWellEq_ + numEq, 0.0});
            getMobilityEval(ebosSimulator, static_cast<int>(subsetPerfID), mob, deferred_logger);

            const auto& fs = fluidState(subsetPerfID);
            setToZero(connPI);

            if (this->isInjector()) {
                this->computeConnLevelInjInd(fs, preferred_phase, connPICalc,
                                             mob, connPI, deferred_logger);
            }
            else {  // Production or zero flow rate
                this->computeConnLevelProdInd(fs, connPICalc, mob, connPI);
            }

            addVector(connPI, wellPI);

            ++subsetPerfID;
            connPI += np;
        }

        // Sum with communication in case of distributed well.
        const auto& comm = this->parallel_well_info_.communication();
        if (comm.size() > 1) {
            comm.sum(wellPI, np);
        }

        assert ((static_cast<int>(subsetPerfID) == this->number_of_perforations_) &&
                "Internal logic error in processing connections for PI/II");
    }



    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    computeWellConnectionDensitesPressures(const Simulator& ebosSimulator,
                                           const WellState& well_state,
                                           const std::vector<double>& b_perf,
                                           const std::vector<double>& rsmax_perf,
                                           const std::vector<double>& rvmax_perf,
                                           const std::vector<double>& surf_dens_perf)
    {
        // Compute densities
        const int nperf = number_of_perforations_;
        const int np = number_of_phases_;
        std::vector<double> perfRates(b_perf.size(),0.0);
        const auto& perf_data = well_state.perfData(this->index_of_well_);
        const auto& perf_rates_state = perf_data.phase_rates;

        for (int perf = 0; perf < nperf; ++perf) {
            for (int comp = 0; comp < np; ++comp) {
                perfRates[perf * num_components_ + comp] =  perf_rates_state[perf * np + ebosCompIdxToFlowCompIdx(comp)];
            }
        }

        if constexpr (has_solvent) {
            const auto& solvent_perf_rates_state = perf_data.solvent_rates;
            for (int perf = 0; perf < nperf; ++perf) {
                perfRates[perf * num_components_ + contiSolventEqIdx] = solvent_perf_rates_state[perf];
            }
        }

        // for producers where all perforations have zero rate we
        // approximate the perforation mixture using the mobility ratio
        // and weight the perforations using the well transmissibility.
        bool all_zero = std::all_of(perfRates.begin(), perfRates.end(), [](double val) { return val == 0.0; });
        if ( all_zero && this->isProducer() ) {
            double total_tw = 0;
            for (int perf = 0; perf < nperf; ++perf) {
                total_tw += well_index_[perf];
            }
            for (int perf = 0; perf < nperf; ++perf) {
                const int cell_idx = well_cells_[perf];
                const auto& intQuants = *(ebosSimulator.model().cachedIntensiveQuantities(cell_idx, /*timeIdx=*/0));
                const auto& fs = intQuants.fluidState();
                const double well_tw_fraction = well_index_[perf] / total_tw;
                double total_mobility = 0.0;
                for (int p = 0; p < np; ++p) {
                    int ebosPhaseIdx = flowPhaseToEbosPhaseIdx(p);
                    total_mobility += fs.invB(ebosPhaseIdx).value() * intQuants.mobility(ebosPhaseIdx).value();
                }
                if constexpr (has_solvent) {
                    total_mobility += intQuants.solventInverseFormationVolumeFactor().value() * intQuants.solventMobility().value();
                }
                for (int p = 0; p < np; ++p) {
                    int ebosPhaseIdx = flowPhaseToEbosPhaseIdx(p);
                    perfRates[perf * num_components_ + p] = well_tw_fraction * intQuants.mobility(ebosPhaseIdx).value() / total_mobility;
                }
                if constexpr (has_solvent) {
                    perfRates[perf * num_components_ + contiSolventEqIdx] = well_tw_fraction * intQuants.solventInverseFormationVolumeFactor().value() / total_mobility;
                }
            }
        }

        this->computeConnectionDensities(perfRates, b_perf, rsmax_perf, rvmax_perf, surf_dens_perf);

        this->computeConnectionPressureDelta();
    }





    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    computeWellConnectionPressures(const Simulator& ebosSimulator,
                                   const WellState& well_state)
    {
         // 1. Compute properties required by computeConnectionPressureDelta().
         //    Note that some of the complexity of this part is due to the function
         //    taking std::vector<double> arguments, and not Eigen objects.
         std::vector<double> b_perf;
         std::vector<double> rsmax_perf;
         std::vector<double> rvmax_perf;
         std::vector<double> surf_dens_perf;
         computePropertiesForWellConnectionPressures(ebosSimulator, well_state, b_perf, rsmax_perf, rvmax_perf, surf_dens_perf);
         computeWellConnectionDensitesPressures(ebosSimulator, well_state, b_perf, rsmax_perf, rvmax_perf, surf_dens_perf);
    }





    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    solveEqAndUpdateWellState(WellState& well_state, DeferredLogger& deferred_logger)
    {
        if (!this->isOperable() && !this->wellIsStopped()) return;

        // We assemble the well equations, then we check the convergence,
        // which is why we do not put the assembleWellEq here.
        BVectorWell dx_well(1);
        dx_well[0].resize(this->numWellEq_);
        this->invDuneD_.mv(this->resWell_, dx_well);

        updateWellState(dx_well, well_state, deferred_logger);
    }





    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    calculateExplicitQuantities(const Simulator& ebosSimulator,
                                const WellState& well_state,
                                DeferredLogger& deferred_logger)
    {
        updatePrimaryVariables(well_state, deferred_logger);
        initPrimaryVariablesEvaluation();
        computeWellConnectionPressures(ebosSimulator, well_state);
        this->computeAccumWell();
    }



    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    apply(const BVector& x, BVector& Ax) const
    {
        if (!this->isOperable() && !this->wellIsStopped()) return;

        if ( param_.matrix_add_well_contributions_ )
        {
            // Contributions are already in the matrix itself
            return;
        }
        assert( this->Bx_.size() == this->duneB_.N() );
        assert( this->invDrw_.size() == this->invDuneD_.N() );

        // Bx_ = duneB_ * x
        this->parallelB_.mv(x, this->Bx_);

        // invDBx = invDuneD_ * Bx_
        // TODO: with this, we modified the content of the invDrw_.
        // Is it necessary to do this to save some memory?
        BVectorWell& invDBx = this->invDrw_;
        this->invDuneD_.mv(this->Bx_, invDBx);

        // Ax = Ax - duneC_^T * invDBx
        this->duneC_.mmtv(invDBx,Ax);
    }




    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    apply(BVector& r) const
    {
        if (!this->isOperable() && !this->wellIsStopped()) return;

        assert( this->invDrw_.size() == this->invDuneD_.N() );

        // invDrw_ = invDuneD_ * resWell_
        this->invDuneD_.mv(this->resWell_, this->invDrw_);
        // r = r - duneC_^T * invDrw_
        this->duneC_.mmtv(this->invDrw_, r);
    }

    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    recoverSolutionWell(const BVector& x, BVectorWell& xw) const
    {
        if (!this->isOperable() && !this->wellIsStopped()) return;

        BVectorWell resWell = this->resWell_;
        // resWell = resWell - B * x
        this->parallelB_.mmv(x, resWell);
        // xw = D^-1 * resWell
        this->invDuneD_.mv(resWell, xw);
    }





    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    recoverWellSolutionAndUpdateWellState(const BVector& x,
                                          WellState& well_state,
                                          DeferredLogger& deferred_logger) const
    {
        if (!this->isOperable() && !this->wellIsStopped()) return;

        BVectorWell xw(1);
        xw[0].resize(this->numWellEq_);

        recoverSolutionWell(x, xw);
        updateWellState(xw, well_state, deferred_logger);
    }




    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    computeWellRatesWithBhp(const Simulator& ebosSimulator,
                            const double& bhp,
                            std::vector<double>& well_flux,
                            DeferredLogger& deferred_logger) const
    {

        const int np = number_of_phases_;
        well_flux.resize(np, 0.0);

        const bool allow_cf = getAllowCrossFlow();

        for (int perf = 0; perf < number_of_perforations_; ++perf) {
            const int cell_idx = well_cells_[perf];
            const auto& intQuants = *(ebosSimulator.model().cachedIntensiveQuantities(cell_idx, /*timeIdx=*/ 0));
            // flux for each perforation
            std::vector<Scalar> mob(num_components_, 0.);
            getMobilityScalar(ebosSimulator, perf, mob, deferred_logger);
            double trans_mult = ebosSimulator.problem().template rockCompTransMultiplier<double>(intQuants, cell_idx);
            const double Tw = well_index_[perf] * trans_mult;

            std::vector<Scalar> cq_s(num_components_, 0.);
            computePerfRateScalar(intQuants, mob, bhp, Tw, perf, allow_cf,
                            cq_s, deferred_logger);

            for(int p = 0; p < np; ++p) {
                well_flux[ebosCompIdxToFlowCompIdx(p)] += cq_s[p];
            }
        }
        this->parallel_well_info_.communication().sum(well_flux.data(), well_flux.size());
    }



    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    computeWellRatesWithBhpPotential(const Simulator& ebosSimulator,
                            const double& bhp,
                            std::vector<double>& well_flux,
                            DeferredLogger& deferred_logger)
    {

        // iterate to get a more accurate well density
        // create a copy of the well_state to use. If the operability checking is sucessful, we use this one
        // to replace the original one
        WellState well_state_copy = ebosSimulator.problem().wellModel().wellState();
        const auto& group_state  = ebosSimulator.problem().wellModel().groupState();

        //  Set current control to bhp, and bhp value in state, modify bhp limit in control object.
        if (well_ecl_.isInjector()) {
            well_state_copy.currentInjectionControl(index_of_well_, Well::InjectorCMode::BHP);
        } else {
            well_state_copy.currentProductionControl(index_of_well_, Well::ProducerCMode::BHP);
        }
        well_state_copy.update_bhp(index_of_well_, bhp);

        const double dt = ebosSimulator.timeStepSize();
        bool converged = this->iterateWellEquations(ebosSimulator, dt, well_state_copy, group_state, deferred_logger);
        if (!converged) {
            const std::string msg = " well " + name() + " did not get converged during well potential calculations "
                                                        "returning zero values for the potential";
            deferred_logger.debug(msg);
            return;
        }
        updatePrimaryVariables(well_state_copy, deferred_logger);
        computeWellConnectionPressures(ebosSimulator, well_state_copy);
        initPrimaryVariablesEvaluation();


        computeWellRatesWithBhp(ebosSimulator, bhp, well_flux, deferred_logger);
    }




    template<typename TypeTag>
    std::vector<double>
    StandardWell<TypeTag>::
    computeWellPotentialWithTHP(const Simulator& ebos_simulator,
                               DeferredLogger& deferred_logger,
                               const WellState &well_state) const
    {
        std::vector<double> potentials(number_of_phases_, 0.0);
        const auto& summary_state = ebos_simulator.vanguard().summaryState();

        const auto& well = well_ecl_;
        if (well.isInjector()){
            const auto& controls = well_ecl_.injectionControls(summary_state);
            auto bhp_at_thp_limit = computeBhpAtThpLimitInj(ebos_simulator, summary_state, deferred_logger);
            if (bhp_at_thp_limit) {
                const double bhp = std::min(*bhp_at_thp_limit, controls.bhp_limit);
                computeWellRatesWithBhp(ebos_simulator, bhp, potentials, deferred_logger);
            } else {
                deferred_logger.warning("FAILURE_GETTING_CONVERGED_POTENTIAL",
                                        "Failed in getting converged thp based potential calculation for well "
                                        + name() + ". Instead the bhp based value is used");
                const double bhp = controls.bhp_limit;
                computeWellRatesWithBhp(ebos_simulator, bhp, potentials, deferred_logger);
            }
        } else {
            computeWellRatesWithThpAlqProd(
                ebos_simulator, summary_state,
                deferred_logger, potentials, getALQ(well_state)
            );
        }

        return potentials;
    }

    template<typename TypeTag>
    double
    StandardWell<TypeTag>::
    computeWellRatesAndBhpWithThpAlqProd(const Simulator &ebos_simulator,
                               const SummaryState &summary_state,
                               DeferredLogger &deferred_logger,
                               std::vector<double> &potentials,
                               double alq) const
    {
        double bhp;
        auto bhp_at_thp_limit = computeBhpAtThpLimitProdWithAlq(
                              ebos_simulator, summary_state, deferred_logger, alq);
        if (bhp_at_thp_limit) {
            const auto& controls = well_ecl_.productionControls(summary_state);
            bhp = std::max(*bhp_at_thp_limit, controls.bhp_limit);
            computeWellRatesWithBhp(ebos_simulator, bhp, potentials, deferred_logger);
        }
        else {
            deferred_logger.warning("FAILURE_GETTING_CONVERGED_POTENTIAL",
                "Failed in getting converged thp based potential calculation for well "
                + name() + ". Instead the bhp based value is used");
            const auto& controls = well_ecl_.productionControls(summary_state);
            bhp = controls.bhp_limit;
            computeWellRatesWithBhp(ebos_simulator, bhp, potentials, deferred_logger);
        }
        return bhp;
    }

    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    computeWellRatesWithThpAlqProd(const Simulator &ebos_simulator,
                               const SummaryState &summary_state,
                               DeferredLogger &deferred_logger,
                               std::vector<double> &potentials,
                               double alq) const
    {
        /*double bhp =*/
        computeWellRatesAndBhpWithThpAlqProd(ebos_simulator,
                                             summary_state,
                                             deferred_logger,
                                             potentials,
                                             alq);
    }

    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    gasLiftOptimizationStage1(
                       WellState& well_state,
                       const GroupState& group_state,
                       const Simulator& ebos_simulator,
                       DeferredLogger& deferred_logger,
                       GLiftProdWells &prod_wells,
                       GLiftOptWells &glift_wells,
                       GLiftWellStateMap &glift_state_map,
                       GasLiftGroupInfo &group_info,
                       GLiftSyncGroups &sync_groups
    ) const
    {
        const auto& summary_state = ebos_simulator.vanguard().summaryState();
        std::unique_ptr<GasLiftSingleWell> glift
            = std::make_unique<GasLiftSingleWell>(
                *this, ebos_simulator, summary_state,
                deferred_logger, well_state, group_state, group_info, sync_groups);
        auto state = glift->runOptimize(
            ebos_simulator.model().newtonMethod().numIterations());
        if (state) {
            glift_state_map.insert({this->name(), std::move(state)});
            glift_wells.insert({this->name(), std::move(glift)});
            return;
        }
        prod_wells.insert({this->name(), this});
    }

    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    computeWellPotentials(const Simulator& ebosSimulator,
                          const WellState& well_state,
                          std::vector<double>& well_potentials,
                          DeferredLogger& deferred_logger) // const
    {
        const int np = number_of_phases_;
        well_potentials.resize(np, 0.0);

        if (this->wellIsStopped()) {
            return;
        }

        // If the well is pressure controlled the potential equals the rate.
        bool thp_controlled_well = false;
        bool bhp_controlled_well = false;
        if (this->isInjector()) {
            const Well::InjectorCMode& current = well_state.currentInjectionControl(index_of_well_);
            if (current == Well::InjectorCMode::THP) {
                thp_controlled_well = true;
            }
            if (current == Well::InjectorCMode::BHP) {
                bhp_controlled_well = true;
            }
        } else {
            const Well::ProducerCMode& current = well_state.currentProductionControl(index_of_well_);
            if (current == Well::ProducerCMode::THP) {
                thp_controlled_well = true;
            }
            if (current == Well::ProducerCMode::BHP) {
                bhp_controlled_well = true;
            }
        }
        if (thp_controlled_well || bhp_controlled_well) {

            double total_rate = 0.0;
            for (int phase = 0; phase < np; ++phase){
                total_rate += well_state.wellRates(index_of_well_)[phase];
            }
            // for pressure controlled wells the well rates are the potentials
            // if the rates are trivial we are most probably looking at the newly
            // opened well and we therefore make the affort of computing the potentials anyway.
            if (std::abs(total_rate) > 0) {
                for (int phase = 0; phase < np; ++phase){
                    well_potentials[phase] = well_state.wellRates(index_of_well_)[phase];
                }
                return;
            }
        }

        // creating a copy of the well itself, to avoid messing up the explicit informations
        // during this copy, the only information not copied properly is the well controls
        StandardWell<TypeTag> well(*this);
        well.calculateExplicitQuantities(ebosSimulator, well_state, deferred_logger);

        // does the well have a THP related constraint?
        const auto& summaryState = ebosSimulator.vanguard().summaryState();
        if (!well.Base::wellHasTHPConstraints(summaryState) || bhp_controlled_well) {
            // get the bhp value based on the bhp constraints
            const double bhp = well.mostStrictBhpFromBhpLimits(summaryState);
            assert(std::abs(bhp) != std::numeric_limits<double>::max());
            well.computeWellRatesWithBhpPotential(ebosSimulator, bhp, well_potentials, deferred_logger);
        } else {
            // the well has a THP related constraint
            well_potentials = well.computeWellPotentialWithTHP(ebosSimulator, deferred_logger, well_state);
        }
    }





    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    updatePrimaryVariables(const WellState& well_state, DeferredLogger& deferred_logger) const
    {
        this->StdWellEval::updatePrimaryVariables(well_state, deferred_logger);
        if (!this->isOperable() && !this->wellIsStopped()) return;

        // other primary variables related to polymer injection
        if constexpr (Base::has_polymermw) {
            if (this->isInjector()) {
                const auto& perf_data = well_state.perfData(this->index_of_well_);
                const auto& water_velocity = perf_data.water_velocity;
                const auto& skin_pressure = perf_data.skin_pressure;
                for (int perf = 0; perf < number_of_perforations_; ++perf) {
                    this->primary_variables_[Bhp + 1 + perf] = water_velocity[perf];
                    this->primary_variables_[Bhp + 1 + number_of_perforations_ + perf] = skin_pressure[perf];
                }
            }
        }
#ifndef NDEBUG
        for (double v : this->primary_variables_) {
            assert(isfinite(v));
        }
#endif
    }




    template<typename TypeTag>
    double
    StandardWell<TypeTag>::
    getRefDensity() const
    {
        return this->perf_densities_[0];
    }




    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    updateWaterMobilityWithPolymer(const Simulator& ebos_simulator,
                                   const int perf,
                                   std::vector<EvalWell>& mob,
                                   DeferredLogger& deferred_logger) const
    {
        const int cell_idx = well_cells_[perf];
        const auto& int_quant = *(ebos_simulator.model().cachedIntensiveQuantities(cell_idx, /*timeIdx=*/ 0));
        const EvalWell polymer_concentration = this->extendEval(int_quant.polymerConcentration());

        // TODO: not sure should based on the well type or injecting/producing peforations
        // it can be different for crossflow
        if (this->isInjector()) {
            // assume fully mixing within injecting wellbore
            const auto& visc_mult_table = PolymerModule::plyviscViscosityMultiplierTable(int_quant.pvtRegionIndex());
            const unsigned waterCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::waterCompIdx);
            mob[waterCompIdx] /= (this->extendEval(int_quant.waterViscosityCorrection()) * visc_mult_table.eval(polymer_concentration, /*extrapolate=*/true) );
        }

        if (PolymerModule::hasPlyshlog()) {
            // we do not calculate the shear effects for injection wells when they do not
            // inject polymer.
            if (this->isInjector() && wpolymer() == 0.) {
                return;
            }
            // compute the well water velocity with out shear effects.
            // TODO: do we need to turn on crossflow here?
            const bool allow_cf = getAllowCrossFlow() || openCrossFlowAvoidSingularity(ebos_simulator);
            const EvalWell& bhp = this->getBhp();

            std::vector<EvalWell> cq_s(num_components_, {this->numWellEq_ + numEq, 0.});
            double perf_dis_gas_rate = 0.;
            double perf_vap_oil_rate = 0.;
            double trans_mult = ebos_simulator.problem().template rockCompTransMultiplier<double>(int_quant, cell_idx);
            const double Tw = well_index_[perf] * trans_mult;
            computePerfRateEval(int_quant, mob, bhp, Tw, perf, allow_cf,
                                cq_s, perf_dis_gas_rate, perf_vap_oil_rate, deferred_logger);
            // TODO: make area a member
            const double area = 2 * M_PI * perf_rep_radius_[perf] * perf_length_[perf];
            const auto& material_law_manager = ebos_simulator.problem().materialLawManager();
            const auto& scaled_drainage_info =
                        material_law_manager->oilWaterScaledEpsInfoDrainage(cell_idx);
            const double swcr = scaled_drainage_info.Swcr;
            const EvalWell poro = this->extendEval(int_quant.porosity());
            const EvalWell sw = this->extendEval(int_quant.fluidState().saturation(FluidSystem::waterPhaseIdx));
            // guard against zero porosity and no water
            const EvalWell denom = max( (area * poro * (sw - swcr)), 1e-12);
            const unsigned waterCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::waterCompIdx);
            EvalWell water_velocity = cq_s[waterCompIdx] / denom * this->extendEval(int_quant.fluidState().invB(FluidSystem::waterPhaseIdx));

            if (PolymerModule::hasShrate()) {
                // the equation for the water velocity conversion for the wells and reservoir are from different version
                // of implementation. It can be changed to be more consistent when possible.
                water_velocity *= PolymerModule::shrate( int_quant.pvtRegionIndex() ) / bore_diameters_[perf];
            }
            const EvalWell shear_factor = PolymerModule::computeShearFactor(polymer_concentration,
                                                                int_quant.pvtRegionIndex(),
                                                                water_velocity);
             // modify the mobility with the shear factor.
            mob[waterCompIdx] /= shear_factor;
        }
    }

    template<typename TypeTag>
    void
    StandardWell<TypeTag>::addWellContributions(SparseMatrixAdapter& jacobian) const
    {
        // We need to change matrx A as follows
        // A -= C^T D^-1 B
        // D is diagonal
        // B and C have 1 row, nc colums and nonzero
        // at (0,j) only if this well has a perforation at cell j.
        typename SparseMatrixAdapter::MatrixBlock tmpMat;
        Dune::DynamicMatrix<Scalar> tmp;
        for ( auto colC = this->duneC_[0].begin(), endC = this->duneC_[0].end(); colC != endC; ++colC )
        {
            const auto row_index = colC.index();

            for ( auto colB = this->duneB_[0].begin(), endB = this->duneB_[0].end(); colB != endB; ++colB )
            {
                Detail::multMatrix(this->invDuneD_[0][0],  (*colB), tmp);
                Detail::negativeMultMatrixTransposed((*colC), tmp, tmpMat);
                jacobian.addToBlock( row_index, colB.index(), tmpMat );
            }
        }
    }





    template<typename TypeTag>
    typename StandardWell<TypeTag>::EvalWell
    StandardWell<TypeTag>::
    pskinwater(const double throughput,
               const EvalWell& water_velocity,
              DeferredLogger& deferred_logger) const
    {
        if constexpr (Base::has_polymermw) {
            const int water_table_id = well_ecl_.getPolymerProperties().m_skprwattable;
            if (water_table_id <= 0) {
                OPM_DEFLOG_THROW(std::runtime_error, "Unused SKPRWAT table id used for well " << name(), deferred_logger);
            }
            const auto& water_table_func = PolymerModule::getSkprwatTable(water_table_id);
            const EvalWell throughput_eval(this->numWellEq_ + numEq, throughput);
            // the skin pressure when injecting water, which also means the polymer concentration is zero
            EvalWell pskin_water(this->numWellEq_ + numEq, 0.0);
            pskin_water = water_table_func.eval(throughput_eval, water_velocity);
            return pskin_water;
        } else {
            OPM_DEFLOG_THROW(std::runtime_error, "Polymermw is not activated, "
                                          "while injecting skin pressure is requested for well " << name(), deferred_logger);
        }
    }





    template<typename TypeTag>
    typename StandardWell<TypeTag>::EvalWell
    StandardWell<TypeTag>::
    pskin(const double throughput,
              const EvalWell& water_velocity,
              const EvalWell& poly_inj_conc,
              DeferredLogger& deferred_logger) const
    {
        if constexpr (Base::has_polymermw) {
            const double sign = water_velocity >= 0. ? 1.0 : -1.0;
            const EvalWell water_velocity_abs = abs(water_velocity);
            if (poly_inj_conc == 0.) {
                return sign * pskinwater(throughput, water_velocity_abs, deferred_logger);
            }
            const int polymer_table_id = well_ecl_.getPolymerProperties().m_skprpolytable;
            if (polymer_table_id <= 0) {
                OPM_DEFLOG_THROW(std::runtime_error, "Unavailable SKPRPOLY table id used for well " << name(), deferred_logger);
            }
            const auto& skprpolytable = PolymerModule::getSkprpolyTable(polymer_table_id);
            const double reference_concentration = skprpolytable.refConcentration;
            const EvalWell throughput_eval(this->numWellEq_ + numEq, throughput);
            // the skin pressure when injecting water, which also means the polymer concentration is zero
            EvalWell pskin_poly(this->numWellEq_ + numEq, 0.0);
            pskin_poly = skprpolytable.table_func.eval(throughput_eval, water_velocity_abs);
            if (poly_inj_conc == reference_concentration) {
                return sign * pskin_poly;
            }
            // poly_inj_conc != reference concentration of the table, then some interpolation will be required
            const EvalWell pskin_water = pskinwater(throughput, water_velocity_abs, deferred_logger);
            const EvalWell pskin = pskin_water + (pskin_poly - pskin_water) / reference_concentration * poly_inj_conc;
            return sign * pskin;
        } else {
            OPM_DEFLOG_THROW(std::runtime_error, "Polymermw is not activated, "
                                          "while injecting skin pressure is requested for well " << name(), deferred_logger);
        }
    }





    template<typename TypeTag>
    typename StandardWell<TypeTag>::EvalWell
    StandardWell<TypeTag>::
    wpolymermw(const double throughput,
               const EvalWell& water_velocity,
               DeferredLogger& deferred_logger) const
    {
        if constexpr (Base::has_polymermw) {
            const int table_id = well_ecl_.getPolymerProperties().m_plymwinjtable;
            const auto& table_func = PolymerModule::getPlymwinjTable(table_id);
            const EvalWell throughput_eval(this->numWellEq_ + numEq, throughput);
            EvalWell molecular_weight(this->numWellEq_ + numEq, 0.);
            if (wpolymer() == 0.) { // not injecting polymer
                return molecular_weight;
            }
            molecular_weight = table_func.eval(throughput_eval, abs(water_velocity));
            return molecular_weight;
        } else {
            OPM_DEFLOG_THROW(std::runtime_error, "Polymermw is not activated, "
                                          "while injecting polymer molecular weight is requested for well " << name(), deferred_logger);
        }
    }





    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    updateWaterThroughput(const double dt, WellState &well_state) const
    {
        if constexpr (Base::has_polymermw) {
            if (this->isInjector()) {
                auto& perf_water_throughput = well_state.perfData(this->index_of_well_).water_throughput;
                for (int perf = 0; perf < number_of_perforations_; ++perf) {
                    const double perf_water_vel = this->primary_variables_[Bhp + 1 + perf];
                    // we do not consider the formation damage due to water flowing from reservoir into wellbore
                    if (perf_water_vel > 0.) {
                        perf_water_throughput[perf] += perf_water_vel * dt;
                    }
                }
            }
        }
    }





    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    handleInjectivityRate(const Simulator& ebosSimulator,
                          const int perf,
                          std::vector<EvalWell>& cq_s) const
    {
        const int cell_idx = well_cells_[perf];
        const auto& int_quants = *(ebosSimulator.model().cachedIntensiveQuantities(cell_idx, /*timeIdx=*/ 0));
        const auto& fs = int_quants.fluidState();
        const EvalWell b_w = this->extendEval(fs.invB(FluidSystem::waterPhaseIdx));
        const double area = M_PI * bore_diameters_[perf] * perf_length_[perf];
        const int wat_vel_index = Bhp + 1 + perf;
        const unsigned water_comp_idx = Indices::canonicalToActiveComponentIndex(FluidSystem::waterCompIdx);

        // water rate is update to use the form from water velocity, since water velocity is
        // a primary variable now
        cq_s[water_comp_idx] = area * this->primary_variables_evaluation_[wat_vel_index] * b_w;
    }




    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    handleInjectivityEquations(const Simulator& ebosSimulator,
                               const WellState& well_state,
                               const int perf,
                               const EvalWell& water_flux_s,
                               DeferredLogger& deferred_logger)
    {
        const int cell_idx = well_cells_[perf];
        const auto& int_quants = *(ebosSimulator.model().cachedIntensiveQuantities(cell_idx, /*timeIdx=*/ 0));
        const auto& fs = int_quants.fluidState();
        const EvalWell b_w = this->extendEval(fs.invB(FluidSystem::waterPhaseIdx));
        const EvalWell water_flux_r = water_flux_s / b_w;
        const double area = M_PI * bore_diameters_[perf] * perf_length_[perf];
        const EvalWell water_velocity = water_flux_r / area;
        const int wat_vel_index = Bhp + 1 + perf;

        // equation for the water velocity
        const EvalWell eq_wat_vel = this->primary_variables_evaluation_[wat_vel_index] - water_velocity;
        this->resWell_[0][wat_vel_index] = eq_wat_vel.value();

        const auto& perf_data = well_state.perfData(this->index_of_well_);
        const auto& perf_water_throughput = perf_data.water_throughput;
        const double throughput = perf_water_throughput[perf];
        const int pskin_index = Bhp + 1 + number_of_perforations_ + perf;

        EvalWell poly_conc(this->numWellEq_ + numEq, 0.0);
        poly_conc.setValue(wpolymer());

        // equation for the skin pressure
        const EvalWell eq_pskin = this->primary_variables_evaluation_[pskin_index]
                                  - pskin(throughput, this->primary_variables_evaluation_[wat_vel_index], poly_conc, deferred_logger);

        this->resWell_[0][pskin_index] = eq_pskin.value();
        for (int pvIdx = 0; pvIdx < this->numWellEq_; ++pvIdx) {
            this->invDuneD_[0][0][wat_vel_index][pvIdx] = eq_wat_vel.derivative(pvIdx+numEq);
            this->invDuneD_[0][0][pskin_index][pvIdx] = eq_pskin.derivative(pvIdx+numEq);
        }

        // the water velocity is impacted by the reservoir primary varaibles. It needs to enter matrix B
        for (int pvIdx = 0; pvIdx < numEq; ++pvIdx) {
            this->duneB_[0][cell_idx][wat_vel_index][pvIdx] = eq_wat_vel.derivative(pvIdx);
        }
    }





    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    checkConvergenceExtraEqs(const std::vector<double>& res,
                             ConvergenceReport& report) const
    {
        // if different types of extra equations are involved, this function needs to be refactored further

        // checking the convergence of the extra equations related to polymer injectivity
        if constexpr (Base::has_polymermw) {
            this->checkConvergencePolyMW(res, report, param_.max_residual_allowed_);
        }
    }





    template<typename TypeTag>
    void
    StandardWell<TypeTag>::
    updateConnectionRatePolyMW(const EvalWell& cq_s_poly,
                               const IntensiveQuantities& int_quants,
                               const WellState& well_state,
                               const int perf,
                               std::vector<RateVector>& connectionRates,
                               DeferredLogger& deferred_logger) const
    {
        // the source term related to transport of molecular weight
        EvalWell cq_s_polymw = cq_s_poly;
        if (this->isInjector()) {
            const int wat_vel_index = Bhp + 1 + perf;
            const EvalWell water_velocity = this->primary_variables_evaluation_[wat_vel_index];
            if (water_velocity > 0.) { // injecting
                const auto& perf_water_throughput = well_state.perfData(this->index_of_well_).water_throughput;
                const double throughput = perf_water_throughput[perf];
                const EvalWell molecular_weight = wpolymermw(throughput, water_velocity, deferred_logger);
                cq_s_polymw *= molecular_weight;
            } else {
                // we do not consider the molecular weight from the polymer
                // going-back to the wellbore through injector
                cq_s_polymw *= 0.;
            }
        } else if (this->isProducer()) {
            if (cq_s_polymw < 0.) {
                cq_s_polymw *= this->extendEval(int_quants.polymerMoleWeight() );
            } else {
                // we do not consider the molecular weight from the polymer
                // re-injecting back through producer
                cq_s_polymw *= 0.;
            }
        }
        connectionRates[perf][this->contiPolymerMWEqIdx] = Base::restrictEval(cq_s_polymw);
    }






    template<typename TypeTag>
    std::optional<double>
    StandardWell<TypeTag>::
    computeBhpAtThpLimitProd(const WellState& well_state,
                             const Simulator& ebos_simulator,
                             const SummaryState& summary_state,
                             DeferredLogger& deferred_logger) const
    {
        return computeBhpAtThpLimitProdWithAlq(ebos_simulator,
                                               summary_state,
                                               deferred_logger,
                                               getALQ(well_state));
    }

    template<typename TypeTag>
    std::optional<double>
    StandardWell<TypeTag>::
    computeBhpAtThpLimitProdWithAlq(const Simulator& ebos_simulator,
                                    const SummaryState& summary_state,
                                    DeferredLogger& deferred_logger,
                                    double alq_value) const
    {
        // Make the frates() function.
        auto frates = [this, &ebos_simulator, &deferred_logger](const double bhp) {
            // Not solving the well equations here, which means we are
            // calculating at the current Fg/Fw values of the
            // well. This does not matter unless the well is
            // crossflowing, and then it is likely still a good
            // approximation.
            std::vector<double> rates(3);
            computeWellRatesWithBhp(ebos_simulator, bhp, rates, deferred_logger);
            return rates;
        };

        return this->StandardWellGeneric<Scalar>::computeBhpAtThpLimitProdWithAlq(frates,
                                                                                  summary_state,
                                                                                  deferred_logger,
                                                                                  alq_value);
    }



    template<typename TypeTag>
    std::optional<double>
    StandardWell<TypeTag>::
    computeBhpAtThpLimitInj(const Simulator& ebos_simulator,
                            const SummaryState& summary_state,
                            DeferredLogger& deferred_logger) const
    {
        // Make the frates() function.
        auto frates = [this, &ebos_simulator, &deferred_logger](const double bhp) {
            // Not solving the well equations here, which means we are
            // calculating at the current Fg/Fw values of the
            // well. This does not matter unless the well is
            // crossflowing, and then it is likely still a good
            // approximation.
            std::vector<double> rates(3);
            computeWellRatesWithBhp(ebos_simulator, bhp, rates, deferred_logger);
            return rates;
        };

        return this->StandardWellGeneric<Scalar>::computeBhpAtThpLimitInj(frates,
                                                                          summary_state,
                                                                          deferred_logger);
    }





    template<typename TypeTag>
    bool
    StandardWell<TypeTag>::
    iterateWellEqWithControl(const Simulator& ebosSimulator,
                             const double dt,
                             const Well::InjectionControls& inj_controls,
                             const Well::ProductionControls& prod_controls,
                             WellState& well_state,
                             const GroupState& group_state,
                             DeferredLogger& deferred_logger)
    {
        const int max_iter = param_.max_inner_iter_wells_;
        int it = 0;
        bool converged;
        do {
            assembleWellEqWithoutIteration(ebosSimulator, dt, inj_controls, prod_controls, well_state, group_state, deferred_logger);

            auto report = getWellConvergence(well_state, Base::B_avg_, deferred_logger);

            converged = report.converged();
            if (converged) {
                break;
            }

            ++it;
            solveEqAndUpdateWellState(well_state, deferred_logger);

            // TODO: when this function is used for well testing purposes, will need to check the controls, so that we will obtain convergence
            // under the most restrictive control. Based on this converged results, we can check whether to re-open the well. Either we refactor
            // this function or we use different functions for the well testing purposes.
            // We don't allow for switching well controls while computing well potentials and testing wells
            // updateWellControl(ebosSimulator, well_state, deferred_logger);
            initPrimaryVariablesEvaluation();
        } while (it < max_iter);

        return converged;
    }


    template<typename TypeTag>
    std::vector<double>
    StandardWell<TypeTag>::
    computeCurrentWellRates(const Simulator& ebosSimulator,
                            DeferredLogger& deferred_logger) const
    {
        // Calculate the rates that follow from the current primary variables.
        std::vector<double> well_q_s(num_components_, 0.);
        const EvalWell& bhp = this->getBhp();
        const bool allow_cf = getAllowCrossFlow() || openCrossFlowAvoidSingularity(ebosSimulator);
        for (int perf = 0; perf < number_of_perforations_; ++perf) {
            const int cell_idx = well_cells_[perf];
            const auto& intQuants = *(ebosSimulator.model().cachedIntensiveQuantities(cell_idx, /*timeIdx=*/ 0));
            std::vector<Scalar> mob(num_components_, 0.);
            getMobilityScalar(ebosSimulator, perf, mob, deferred_logger);
            std::vector<Scalar> cq_s(num_components_, 0.);
            double trans_mult = ebosSimulator.problem().template rockCompTransMultiplier<double>(intQuants,  cell_idx);
            const double Tw = well_index_[perf] * trans_mult;
            computePerfRateScalar(intQuants, mob, bhp.value(), Tw, perf, allow_cf,
                            cq_s, deferred_logger);
            for (int comp = 0; comp < num_components_; ++comp) {
                well_q_s[comp] += cq_s[comp];
            }
        }
        const auto& comm = this->parallel_well_info_.communication();
        if (comm.size() > 1)
        {
            comm.sum(well_q_s.data(), well_q_s.size());
        }
        return well_q_s;
    }





    template <typename TypeTag>
    void
    StandardWell<TypeTag>::
    computeConnLevelProdInd(const typename StandardWell<TypeTag>::FluidState& fs,
                            const std::function<double(const double)>& connPICalc,
                            const std::vector<EvalWell>& mobility,
                            double* connPI) const
    {
        const auto& pu = this->phaseUsage();
        const int   np = this->number_of_phases_;
        for (int p = 0; p < np; ++p) {
            // Note: E100's notion of PI value phase mobility includes
            // the reciprocal FVF.
            const auto connMob =
                mobility[ flowPhaseToEbosCompIdx(p) ].value()
                    * fs.invB(flowPhaseToEbosPhaseIdx(p)).value();

            connPI[p] = connPICalc(connMob);
        }

        if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx) &&
            FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx))
        {
            const auto io = pu.phase_pos[Oil];
            const auto ig = pu.phase_pos[Gas];

            const auto vapoil = connPI[ig] * fs.Rv().value();
            const auto disgas = connPI[io] * fs.Rs().value();

            connPI[io] += vapoil;
            connPI[ig] += disgas;
        }
    }





    template <typename TypeTag>
    void
    StandardWell<TypeTag>::
    computeConnLevelInjInd(const typename StandardWell<TypeTag>::FluidState& fs,
                           const Phase preferred_phase,
                           const std::function<double(const double)>& connIICalc,
                           const std::vector<EvalWell>& mobility,
                           double* connII,
                           DeferredLogger& deferred_logger) const
    {
        // Assumes single phase injection
        const auto& pu = this->phaseUsage();

        auto phase_pos = 0;
        if (preferred_phase == Phase::GAS) {
            phase_pos = pu.phase_pos[Gas];
        }
        else if (preferred_phase == Phase::OIL) {
            phase_pos = pu.phase_pos[Oil];
        }
        else if (preferred_phase == Phase::WATER) {
            phase_pos = pu.phase_pos[Water];
        }
        else {
            OPM_DEFLOG_THROW(NotImplemented,
                             "Unsupported Injector Type ("
                             << static_cast<int>(preferred_phase)
                             << ") for well " << this->name()
                             << " during connection I.I. calculation",
                             deferred_logger);
        }

        const auto zero   = EvalWell { this->numWellEq_ + this->numEq, 0.0 };
        const auto mt     = std::accumulate(mobility.begin(), mobility.end(), zero);
        connII[phase_pos] = connIICalc(mt.value() * fs.invB(flowPhaseToEbosPhaseIdx(phase_pos)).value());
    }
} // namespace Opm
