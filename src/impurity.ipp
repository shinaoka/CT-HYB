#include "impurity.hpp"

template<typename IMP_MODEL>
void HybridizationSimulation<IMP_MODEL>::define_parameters(parameters_type &parameters) {
  Base::define_parameters(parameters);

  alps::define_convenience_parameters(parameters);
  parameters
      .description("Continous-time hybridization expansion impurity solver")
      .define<double>("TIME_LIMIT", "Total simulation time (in units of second)")
      .define<double>("THERMALIZATION_TIME",
                      -1,
                      "Thermalization time (in units of second). The default value is 25 % of the total simulation time.")
      .define<int>("Tmin", 1, "The scheduler checks longer than every Tmin seconds if the simulation is finished.")
      .define<int>("Tmax", 60, "The scheduler checks shorter than every Tmax seconds if the simulation is finished.")
      .define<int>("VERBOSE", 0, "If VERBOSE is not zero, more messages are generated.")
      //Model definition
      .define<int>("MODEL.SITES", "Number of sites/orbitals")
      .define<int>("MODEL.SPINS", "Number of spins")
      .define<double>("MODEL.BETA", "Inverse temperature")
      .define<int>("MODEL.N_TAU_HYB", "Hybridization function is defined on a uniform mesh of N_TAU + 1 imaginary points.")
      //Updates
      .define<int>("UPDATE.MULTI_PAIR_INS_REM", 1, "1 for only single-pair update. k for up to k-pair update.")
      .define<int>("UPDATE.N_GLOBAL_UPDATES", 10, "Global updates are performed every N_GLOBAL_UPDATES updates.")
      .define<std::string>("UPDATE.SWAP_VECTOR", "", "Definition of global flavor-exchange updates.")
      //Measurement
      .define<int>("MEASUREMENT.N_MEAS", 10, "Expensive measurements are performed every N_MEAS updates.")
      //
      //Single-particle GF
      .define<int>("MEASUREMENT.G1.N_LEGENDRE", 100, "Number of legendre coefficients for measuring G(tau)")
      .define<int>("MEASUREMENT.G1.N_TAU", 2000, "G(tau) is computed on a uniform mesh of MEASUREMENT.G1.N_TAU + 1 points.")
      .define<int>("MEASUREMENT.G1.N_MATSUBARA", 2000, "G(i omega_n) is computed on a uniform mesh of MEASUREMENT.G1.N_OMEGA Matsubara frequencies.")
      //
      //Two-time two-particle GF
      .define<int>("MEASUREMENT.TWO_TIME_G2.ON", 0, "Set a non-zero value to activate measurement.")
      .define<int>("MEASUREMENT.TWO_TIME_G2.N_LEGENDRE", 50, "Number of legendre coefficients for measuring two-time two-particle Green's function.")
      //
      //Equal-time two-particle GF
      .define<int>("MEASUREMENT.EQUAL_TIME_G2.ON", 0, "Set a non-zero value to activate measurement.")
      //
      //Density-density correlations
      .define<std::string>("MEASUREMENT.NN_CORR.DEF", "", "Input file for definition of density-density correlation functions")
      .define<int>("MEASUREMENT.NN_CORR.N_TAU", 0, "Number of imaginary time points for measurement (tau=0, ...., beta/2)")
      .define<int>("MEASUREMENT.MAX_ORDER_HISTOGRAM", 1000, "Expansion order (per flavor) up to which histogram is measured.");

  IMP_MODEL::define_parameters(parameters);
}


template<typename IMP_MODEL>
HybridizationSimulation<IMP_MODEL>::HybridizationSimulation(parameters_type const &p, int rank)
    : alps::mcbase(p, rank),
      par(p),
      BETA(parameters["MODEL.BETA"]),      //inverse temperature
      SITES(parameters["MODEL.SITES"]),          //number of sites
      SPINS(parameters["MODEL.SPINS"]),          //number of spins
      FLAVORS(SPINS * SITES),                             //flavors, i.e. #spins * #sites
      N(static_cast<int>(parameters["MODEL.N_TAU_HYB"])),                  //time slices
      Np1(N + 1),
      N_meas(parameters["MEASUREMENT.N_MEAS"]),
      thermalization_time(parameters["THERMALIZATION_TIME"]),
      start_time(time(NULL)),
      p_model(new IMP_MODEL(p, rank == 0)),//impurity model
      F(new HybridizationFunction<SCALAR>(
          BETA, N, FLAVORS, p_model->get_F()
        )
      ),
#ifdef ALPS_HAVE_MPI
      comm(),
#endif
      N_win_standard(1),
      sweeps(0),                                                                 //sweeps done up to now
      mc_config(F),
      config_space_extra_weight(0),
      worm_space_extra_weight_map(),
      operator_pair_flavor_updater(FLAVORS),
      single_op_shift_updater(BETA, FLAVORS, N),
      worm_movers(0),
      worm_insertion_removers(0),
      sliding_window(p_model.get(), BETA),
      g_meas_legendre(FLAVORS, p["MEASUREMENT.G1.N_LEGENDRE"], N, BETA),
      p_meas_corr(0),
      global_shift_acc_rate(),
      swap_acc_rate(0),
      timings(4, 0.0),
      verbose(p["VERBOSE"].template as<int>() != 0),
      thermalized(false),
      pert_order_recorder() {

  if (thermalization_time < 0) {
    thermalization_time = static_cast<double>(0.25 * parameters["TIME_LIMIT"].template as<long>());
  }
  if (thermalization_time > 0.9 * parameters["TIME_LIMIT"].template as<double>()) {
    throw std::runtime_error("TIME_LIMIT is too short in comparison with THERMALIZATION_TIME.");
  }

  /////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////
  ////Vectors Initialization Part//////////////////////////////////////
  ////Resize Vectors and Matrices so they have the right size./////////
  /////////////////////////////////////////////////////////////////////
  resize_vectors();

  /////////////////////////////////////////////////////////////////////
  ////Initialize Monte Carlo configuration  ///////////////////////////
  /////////////////////////////////////////////////////////////////////
  //if (p["SLIDING_WINDOW.MAX"].template as<int>() < 1) {
    //throw std::runtime_error("SLIDING_WINDOW.MAX cannot be smaller than 1.");
  //}
  //if (p["SLIDING_WINDOW.MAX"].template as<int>() < p["SLIDING_WINDOW.MAX"].template as<int>()) {
    //throw std::runtime_error("SLIDING_WINDOW.MAX cannot be smaller than SLIDING_WINDOW.MAX.");
  //}
  //sliding_window.init_stacks(p["SLIDING_WINDOW.MAX"], mc_config.operators);
  sliding_window.init_stacks(10000, mc_config.operators);
  mc_config.trace = sliding_window.compute_trace(mc_config.operators);
  if (global_mpi_rank == 0 && verbose) {
    std::cout << "initial trace = " << mc_config.trace << " with N_SLIDING_WINDOW = " << sliding_window.get_n_window()
        << std::endl;
  }

  //Equal-time two-particle Green's function
  read_eq_time_two_particle_greens_meas();

  //Two-time correlation functions
  read_two_time_correlation_functions();

  if (global_mpi_rank == 0 && verbose) {
    std::cout << "The number of blocks in the inverse matrix is " << mc_config.M.num_blocks() << "." << std::endl;
    for (int block = 0; block < mc_config.M.num_blocks(); ++block) {
      std::cout << "flavors in block " << block << " : ";
      for (int flavor = 0; flavor < mc_config.M.num_flavors(block); ++flavor) {
        std::cout << mc_config.M.flavors(block)[flavor] << " ";
      }
      std::cout << std::endl;
    }
  }

  const int rank_ins_rem = par["UPDATE.MULTI_PAIR_INS_REM"].template as<int>();
  if (rank_ins_rem < 1) {
    throw std::runtime_error("UPDATE.MULTI_PAIR_INS_REM is not valid.");
  }
  for (int k = 1; k < rank_ins_rem + 1; ++k) {
    typedef InsertionRemovalUpdater<SCALAR, EXTENDED_SCALAR, SW_TYPE> TypeOffDiag;
    typedef InsertionRemovalDiagonalUpdater<SCALAR, EXTENDED_SCALAR, SW_TYPE> TypeDiag;
    ins_rem_updater.push_back(
        boost::shared_ptr<TypeOffDiag>(
            new TypeOffDiag(k, FLAVORS)
        )
    );
    ins_rem_diagonal_updater.push_back(
        boost::shared_ptr<TypeDiag>(
            new TypeDiag(k, FLAVORS, BETA, N / 2)
        )
    );
  }

  create_worm_updaters();

  create_observables();
}


template<typename IMP_MODEL>
bool HybridizationSimulation<IMP_MODEL>::is_thermalized() const {
  return thermalized;
}

template<typename IMP_MODEL>
double HybridizationSimulation<IMP_MODEL>::fraction_completed() const {
  //double work =
  //(is_thermalized() ? (sweeps - thermalization_checker.get_actual_thermalization_steps()) / double(total_sweeps)
  //: 0.);
  //if (work > 1.0) {
  //work = 1.0;
  //}
  //return work;
  return 0.0;
}

template<typename IMP_MODEL>
void HybridizationSimulation<IMP_MODEL>::update() {
#ifdef MEASURE_TIMING
  boost::timer::cpu_timer timer;
#endif

  //////////////////////////////////
  // Monte Carlo updates
  //////////////////////////////////
  for (int imeas = 0; imeas < N_meas; imeas++) {    // accumulate measurements from N_meas updates before storing
    sweeps++;

    pert_order_recorder << mc_config.pert_order();

#ifdef MEASURE_TIMING
    const double time1 = timer.elapsed().wall * 1E-9;
#endif

    /** one sweep of the window */
    do_one_sweep();

#ifdef MEASURE_TIMING
    const double time2 = timer.elapsed().wall * 1E-9;
    timings[0] += time2 - time1;
#endif

    //Perform global updates which might cost O(beta)
    //Ex: flavor exchanges, global shift
    if (sweeps % par["UPDATE.N_GLOBAL_UPDATES"].template as<int>() == 0) {
      global_updates();
    }

    //update parameters for MC moves and window size
    if (!is_thermalized()) {
      update_MC_parameters();
    }

#ifdef MEASURE_TIMING
    const double time3 = timer.elapsed().wall * 1E-9;
    timings[1] += time3 - time2;
#endif

    if (is_thermalized()) {
      measure_every_step();
    }

#ifdef MEASURE_TIMING
    const double time4 = timer.elapsed().wall * 1E-9;
    timings[2] += time4 - time3;
#endif

    sanity_check();
  }//loop up to N_meas
}

template<typename IMP_MODEL>
void HybridizationSimulation<IMP_MODEL>::measure_every_step() {
  assert(is_thermalized());

  switch (mc_config.current_config_space()) {
    case Z_FUNCTION:
      g_meas_legendre.measure(mc_config);//measure Green's function by removal
      measure_scalar_observable<SCALAR>(measurements, "kLkR",
                                        static_cast<double>(measure_kLkR(mc_config.operators, BETA,
                                                                         0.5 * BETA * random())) * mc_config.sign);
      measure_scalar_observable<SCALAR>(measurements,
                                        "k",
                                        static_cast<double>(mc_config.operators.size()) * mc_config.sign);
      break;

    case G1:
      //p_G1_meas->measure(mc_config, measurements, random, sliding_window, N_win_standard, "G1");
      p_G1_meas->measure_via_hyb(mc_config, measurements, random, sliding_window, "G1");
      break;

    case Two_time_G2:
      p_two_time_G2_meas->measure(mc_config,
                         measurements,
                         random,
                         sliding_window,
                         N_win_standard,
                         "Two_time_G2");
      break;

    case Equal_time_G1:
      p_equal_time_G1_meas->measure_G1(mc_config, measurements, "Equal_time_G1");
      break;

    case Equal_time_G2:
      p_equal_time_G2_meas->measure_G2(mc_config, measurements, "Equal_time_G2");
      break;

    default:
      throw std::runtime_error("Used unsupported worm");
  }
  //measure configuration space volume
  num_steps_in_config_space[get_config_space_position(mc_config.current_config_space())] += 1.0;
}

template<typename IMP_MODEL>
void HybridizationSimulation<IMP_MODEL>::measure() {
  assert(is_thermalized());
#ifdef MEASURE_TIMING
  boost::timer::cpu_timer timer;
#endif

  //Measure the volumes of the configuration spaces
  {
    measurements["Z_function_space_num_steps"] << num_steps_in_config_space[0];
    for (int w = 0; w < worm_types.size(); ++w) {
      measurements["worm_space_num_steps_" + get_config_space_name(worm_types[w])] << num_steps_in_config_space[w + 1];
    }

    num_steps_in_config_space /= config_space_extra_weight;
    measurements["Z_function_space_volume"] << num_steps_in_config_space[0];
    for (int w = 0; w < worm_types.size(); ++w) {
      measurements["worm_space_volume_" + get_config_space_name(worm_types[w])] << num_steps_in_config_space[w + 1];
    }

    std::fill(num_steps_in_config_space.begin(), num_steps_in_config_space.end(), 0.0);
  }

  //Acceptance rate
  {
    for (int i = 0; i < worm_insertion_removers.size(); ++i) {
      worm_insertion_removers[i]->measure_acc_rate(measurements);
    }
    for (int i = 0; i < worm_movers.size(); ++i) {
      worm_movers[i]->measure_acc_rate(measurements);
    }
    for (typename std::map<std::string, boost::shared_ptr<LocalUpdaterType> >::iterator
             it = specialized_updaters.begin(); it != specialized_updaters.end(); ++ it) {
      it->second->measure_acc_rate(measurements);
    }
  }

  if (mc_config.current_config_space() == Z_FUNCTION) {
    assert(!mc_config.p_worm);
    measure_Z_function_space();
  } else if (mc_config.current_config_space() == Two_time_G2) {
    //measure_N2_space();
  }

#ifdef MEASURE_TIMING
  timings[3] = timer.elapsed().wall * 1E-9;
  measurements["TimingsSecPerNMEAS"] << timings;
  std::fill(timings.begin(), timings.end(), 0.0);
#endif
}

template<typename IMP_MODEL>
void HybridizationSimulation<IMP_MODEL>::measure_Z_function_space() {
  // measure the perturbation order
  {
    const std::vector<int> &order_creation_flavor = count_creation_operators(FLAVORS, mc_config);
    const int N_order = par["MEASUREMENT.MAX_ORDER_HISTOGRAM"].template as<int>();
    for (int flavor = 0; flavor < FLAVORS; ++flavor) {
      std::vector<double> order_creation_meas(FLAVORS * N_order,
                                              0.0);
      if (order_creation_flavor[flavor] < N_order) {
        order_creation_meas[flavor * N_order + order_creation_flavor[flavor]] = 1.0;
      }
      measurements["order"] << order_creation_meas;
    }
    {
      std::vector<double> tmp;
      std::copy(order_creation_flavor.begin(), order_creation_flavor.end(), std::back_inserter(tmp));
      measurements["PerturbationOrderFlavors"] << tmp;
    }
  }

  single_op_shift_updater.measure_acc_rate(measurements);
  for (int k = 1; k < par["UPDATE.MULTI_PAIR_INS_REM"].template as<int>() + 1; ++k) {
    ins_rem_diagonal_updater[k - 1]->measure_acc_rate(measurements);
  }

  operator_pair_flavor_updater.measure_acc_rate(measurements);

  //measure acceptance rate of global shift
  if (global_shift_acc_rate.has_samples()) {
    measurements["Acceptance_rate_global_shift"] << global_shift_acc_rate.compute_acceptance_rate();
    global_shift_acc_rate.reset();
  }

  //measure acceptance rate of swap update
  if (swap_acc_rate.size() > 0 && swap_acc_rate[0].has_samples()) {
    std::vector<double> acc_swap(swap_acc_rate.size());
    for (int iupdate = 0; iupdate < swap_acc_rate.size(); ++iupdate) {
      assert(swap_acc_rate[iupdate].has_samples());
      acc_swap[iupdate] = swap_acc_rate[iupdate].compute_acceptance_rate();
      swap_acc_rate[iupdate].reset();
    }
    measurements["Acceptance_rate_swap"] << acc_swap;
  }

  //Measure <n>
  measure_n();

  //Measure <n>
  measure_two_time_correlation_functions();

  //Measure Legendre coefficients of single-particle Green's function
  if (g_meas_legendre.has_samples()) {
    measure_simple_vector_observable<COMPLEX>(measurements, "Greens_legendre",
                                              to_std_vector(
                                                  g_meas_legendre.get_measured_legendre_coefficients(p_model->get_rotmat_Delta())
                                              )
    );
    measure_simple_vector_observable<COMPLEX>(measurements, "Greens_legendre_rotated",
                                              to_std_vector(
                                                  g_meas_legendre.get_measured_legendre_coefficients(Eigen::Matrix<
                                                      SCALAR,
                                                      Eigen::Dynamic,
                                                      Eigen::Dynamic>::Identity(FLAVORS, FLAVORS))
                                              )
    );
    g_meas_legendre.reset();
  }

  measurements["Sign"] << mycast<double>(mc_config.sign);

}

//Measure the expectation values of density operators
template<typename IMP_MODEL>
void HybridizationSimulation<IMP_MODEL>::measure_n() {
  assert(is_thermalized());
  MeasStaticObs<SlidingWindowManager<IMP_MODEL>, CdagC> meas(sliding_window, mc_config.operators);
  std::vector<CdagC> ops(FLAVORS);
  std::vector<EXTENDED_COMPLEX> result_meas(FLAVORS);
  for (int flavor = 0; flavor < FLAVORS; ++flavor) {
    boost::array<int, 2> flavors_tmp;
    flavors_tmp[0] = flavor;
    flavors_tmp[1] = flavor;
    ops[flavor] = CdagC(flavors_tmp);
  }

  //Measure <n>
  meas.perform_meas(ops, result_meas);

  //We measure only the real part because the Monte Carl average of a density operator should be real.
  // <n> = <n>_MC/<sign>_MC: <sign>_MC=real, <n>_MC=real, <n>=real
  //Note: we must take the real part of the quantity after it's multiplied by "sign/trace".
  std::vector<double> result_meas_Re(FLAVORS);
  EXTENDED_COMPLEX inv_trace = static_cast<EXTENDED_SCALAR>(EXTENDED_SCALAR(1.0) / mc_config.trace);
  for (int flavor = 0; flavor < FLAVORS; ++flavor) {
    result_meas_Re[flavor] =
        convert_to_scalar(static_cast<EXTENDED_REAL>(get_real(result_meas[flavor] * mc_config.sign * inv_trace)));
  }
  measurements["n"] << result_meas_Re;
}

//Measure two-time correlation functions by insertion
template<typename IMP_MODEL>
void HybridizationSimulation<IMP_MODEL>::measure_two_time_correlation_functions() {
  assert(is_thermalized());
  if (p_meas_corr.get() == 0) {
    return;
  }

  boost::multi_array<EXTENDED_COMPLEX, 2> result;
  p_meas_corr->perform_meas(sliding_window, mc_config.operators, result);
  const EXTENDED_COMPLEX coeff = EXTENDED_COMPLEX(mc_config.sign) / EXTENDED_COMPLEX(mc_config.trace);
  std::transform(result.origin(), result.origin() + result.num_elements(), result.origin(),
                 std::bind1st(std::multiplies<EXTENDED_COMPLEX>(), coeff));

  measure_simple_vector_observable<COMPLEX>(measurements,
                                            "Two_time_correlation_functions",
                                            to_complex_double_std_vector(result));
}

//for std::random_shuffle
class MyRandomNumberGenerator: public std::unary_function<unsigned int, unsigned int> {
 public:
  MyRandomNumberGenerator(alps::random01 &random) : random_(random) { };
  unsigned int operator()(unsigned int N) {
    return static_cast<unsigned int>(N * random_());
  }

 private:
  alps::random01 &random_;
};

template<typename IMP_MODEL>
void HybridizationSimulation<IMP_MODEL>::do_one_sweep() {
  assert(sliding_window.get_position_right_edge() == 0);

  boost::random::uniform_int_distribution<> dist(1, par["UPDATE.MULTI_PAIR_INS_REM"].template as<int>());
  const int rank_ins_rem = dist(random.engine());
  const int current_n_window = std::max(N_win_standard / rank_ins_rem, 1);
  if (current_n_window != sliding_window.get_n_window()) {
    sliding_window.set_window_size(current_n_window, mc_config.operators, 0, ITIME_LEFT);
  }

  assert(sliding_window.get_position_right_edge() == 0);
  const int num_move = std::max(4 * current_n_window - 4, 1);
  for (int move = 0; move < num_move; ++move) {
    double pert_order_sum = 0;
    //insertion and removal of operators hybridized with the bath
    for (int update = 0; update < FLAVORS; ++update) {
      ins_rem_updater[rank_ins_rem - 1]->update(random, BETA, mc_config, sliding_window);
      sanity_check();//for debug
      ins_rem_diagonal_updater[rank_ins_rem - 1]->update(random, BETA, mc_config, sliding_window);
      sanity_check();//for debug
      operator_pair_flavor_updater.update(random, BETA, mc_config, sliding_window);
      pert_order_sum += mc_config.pert_order();
    }

    //shift move of operators hybridized with the bath
    for (int update = 0; update < FLAVORS * rank_ins_rem; ++update) {
      single_op_shift_updater.update(random, BETA, mc_config, sliding_window);
    }

    transition_between_config_spaces();

    sliding_window.move_window_to_next_position(mc_config.operators);
  }
  sanity_check();
  assert(sliding_window.get_position_right_edge() == 0);
}

template<typename IMP_MODEL>
void HybridizationSimulation<IMP_MODEL>::transition_between_config_spaces() {
  //Worm insertion/removal
  if (worm_types.size() == 0) {
    return;
  }

  for (int update = 0; update < FLAVORS; ++update) {
    //worm insertion and removal
    if (mc_config.current_config_space() == Z_FUNCTION) {
      const int i_worm = static_cast<int>(random() * worm_insertion_removers.size());
      worm_insertion_removers[i_worm]->update(random, BETA, mc_config, sliding_window, worm_space_extra_weight_map);
    } else {
      const int i_worm = get_config_space_position(mc_config.current_config_space()) - 1;
      assert (i_worm >= 0);
      worm_insertion_removers[i_worm]->update(random, BETA, mc_config, sliding_window, worm_space_extra_weight_map);
    }
    adjust_worm_space_weight();

    //G1 worm insertion and removal by changing hybridization lines
    if (mc_config.current_config_space() == Z_FUNCTION || mc_config.current_config_space() == G1) {
      specialized_updaters["G1_ins_rem_hyb"]->update(random, BETA, mc_config, sliding_window, worm_space_extra_weight_map);
      adjust_worm_space_weight();
    }

    //EqualTimeG1 <=> TwoTimeG2
    if (specialized_updaters.find("Connect_Equal_time_G1_and_Two_time_G2") != specialized_updaters.end() &&
        (mc_config.current_config_space() == Equal_time_G1 || mc_config.current_config_space() == Two_time_G2)
        ) {
      bool accepted = specialized_updaters["Connect_Equal_time_G1_and_Two_time_G2"]->
          update(random, BETA, mc_config, sliding_window, worm_space_extra_weight_map);
      adjust_worm_space_weight();
    }


    if (specialized_updaters.find("G1_shifter_hyb") != specialized_updaters.end()
        && mc_config.current_config_space() == G1) {
      bool accepted = specialized_updaters["G1_shifter_hyb"]->
          update(random, BETA, mc_config, sliding_window, worm_space_extra_weight_map);
      //std::cout << "accepted " << accepted << std::endl;
      adjust_worm_space_weight();
    }

    //worm move
    const int i_config_space = get_config_space_position(mc_config.current_config_space());
    if (i_config_space > 0) {
      worm_movers[i_config_space - 1]->update(random, BETA, mc_config, sliding_window, worm_space_extra_weight_map);
    }
  }
}

template<typename IMP_MODEL>
void HybridizationSimulation<IMP_MODEL>::global_updates() {
  const std::size_t n_sliding_window_bak = sliding_window.get_n_window();
  sliding_window.set_window_size(1, mc_config.operators);

  //jump between configuration spaces without a window
  transition_between_config_spaces();

  std::vector<SCALAR> det_vec = mc_config.M.compute_determinant_as_product();

  //Swap flavors
  if (swap_vector.size() > 0) {
    //do updates randomly
    std::vector<int> execute_ordering;
    for (int i = 0; i < swap_vector.size(); ++i) {
      execute_ordering.push_back(i);
    }
    MyRandomNumberGenerator rnd(random);
    std::random_shuffle(execute_ordering.begin(), execute_ordering.end(), rnd);

    for (int itry = 0; itry < swap_vector.size(); ++itry) {
      const int iupdate = execute_ordering[itry];
      const bool accepted = global_update<SCALAR, EXTENDED_SCALAR>(random, BETA,
                                                                   mc_config,
                                                                   det_vec,
                                                                   sliding_window,
                                                                   FLAVORS,
                                                                   ExchangeFlavor(&swap_vector[iupdate].first[0]),
                                                                   WormExchangeFlavor(&swap_vector[iupdate].first[0]),
                                                                   std::max(N_win_standard, 10)
      );

      if (accepted) {
        swap_acc_rate[iupdate].accepted();
        sanity_check();
      } else {
        swap_acc_rate[iupdate].rejected();
        sanity_check();
      }
    }
  }

  //Shift operators to restore translational symmetry
  {
    const double shift = random() * BETA;
    const bool accepted = global_update<SCALAR, EXTENDED_SCALAR>(random, BETA,
                                                                 mc_config,
                                                                 det_vec,
                                                                 sliding_window,
                                                                 FLAVORS,
                                                                 OperatorShift(BETA, shift),
                                                                 WormShift(BETA, shift),
                                                                 std::max(N_win_standard, 10)
    );
    if (accepted) {
      global_shift_acc_rate.accepted();
      mc_config.check_nan();
    } else {
      global_shift_acc_rate.rejected();
      if (p_model->translationally_invariant()) {
        std::cerr << "Warning: a global shift is rejected!" << std::endl;
      }
    }
    sanity_check();
  }

  sliding_window.set_window_size(n_sliding_window_bak, mc_config.operators, 0, ITIME_LEFT);
  sanity_check();
}

template<typename IMP_MODEL>
void HybridizationSimulation<IMP_MODEL>::update_MC_parameters() {
  assert(!is_thermalized());
  if (is_thermalized()) {
    throw std::logic_error("called update_MC_parameters after thermalized");
  }

  //record expansion order
  static std::list<double> min_pert_order_hist;
  min_pert_order_hist.push_back(mc_config.pert_order());
  if (min_pert_order_hist.size() > 20) {
    min_pert_order_hist.pop_front();
  }
  const double min_expansion_order_ave =
      std::accumulate(min_pert_order_hist.begin(), min_pert_order_hist.end(), 0.0) / min_pert_order_hist.size();

  //new window size for single-pair insertion and removal update
  N_win_standard = static_cast<std::size_t>(
      std::max(
          par["SLIDING_WINDOW.MAX"].template as<int>(),
          std::min(
              static_cast<int>(std::ceil(min_expansion_order_ave / FLAVORS)),
              par["SLIDING_WINDOW.MAX"].template as<int>()
          )

      )
  );
  if (verbose && global_mpi_rank == 0 && sweeps % 10 == 0) {
    std::cout << " new window size = " << N_win_standard << " sweep = " << sweeps << " pert_order = "
        << mc_config.pert_order() << std::endl;
  }

  //Update parameters for single-operator shift updates
  single_op_shift_updater.update_parameters();

  //check if thermalization is checked
  if (time(NULL) - start_time > thermalization_time) {
    thermalized = true;
  }
}

/////////////////////////////////////////////////
// Something to be done before measurement
/////////////////////////////////////////////////
template<typename IMP_MODEL>
void HybridizationSimulation<IMP_MODEL>::prepare_for_measurement() {
  g_meas_legendre.reset();
  single_op_shift_updater.finalize_learning();
  for (int k = 1; k < par["UPDATE.MULTI_PAIR_INS_REM"].template as<int>() + 1; ++k) {
    ins_rem_diagonal_updater[k - 1]->finalize_learning();
  }

  for (int w = 0; w < worm_types.size(); ++w) {
    worm_insertion_removers[w]->finalize_learning();
  }
  for (typename std::map<std::string, boost::shared_ptr<LocalUpdaterType> >::iterator it = specialized_updaters.begin();
       it != specialized_updaters.end(); ++it) {
    it->second->finalize_learning();
  }

  if (global_mpi_rank == 0) {
    std::cout << "Thermalization process done after " << sweeps << " steps." << std::endl;
    std::cout << "The number of segments for sliding window update is " << N_win_standard << "."
        << std::endl;
    std::cout << "Perturbation orders (averaged over processes) are the following:" << std::endl;
  }
  const std::vector<int> &order_creation_flavor = count_creation_operators(FLAVORS, mc_config);
  if (global_mpi_rank == 0) {
    for (int flavor = 0; flavor < FLAVORS; ++flavor) {
      std::cout << " flavor " << flavor << " " << order_creation_flavor[flavor] << std::endl;
    }
    std::cout << std::endl;
  }
  if (p_flat_histogram_config_space) {
    if (!p_flat_histogram_config_space->converged()) {
      std::cout <<
          boost::format(
              "Warning: flat histogram is not yet obtained for MPI rank %1%. Increase thermalization time!"
          ) % global_mpi_rank << std::endl;
    }
    p_flat_histogram_config_space->finish_learning(false);
  }
  measurements["Pert_order_start"] << pert_order_recorder.mean();

  if (verbose) {
    std::cout << std::endl << "Weight of configuration spaces for MPI rank " << global_mpi_rank << " : ";
    std::cout << " Z function space = " << config_space_extra_weight[0];
    for (int w = 0; w < worm_types.size(); ++w) {
      std::cout << " , " << get_config_space_name(worm_types[w]) << " = " << config_space_extra_weight[w + 1];
    }
    std::cout << std::endl;
  }
}

template<typename IMP_MODEL>
void HybridizationSimulation<IMP_MODEL>::finish_measurement() {
  measurements["Pert_order_end"] << pert_order_recorder.mean();
}

/**
 * Transform the single-particle Green's function back to the original basis
 */
template<typename MAT, typename MAT_COMPLEX, typename COMPLEX>
void
transform_G_back_to_original_basis(int FLAVORS,
                                   int SITES,
                                   int SPINS,
                                   int Np1,
                                   const MAT &rotmat_Delta,
                                   const MAT &inv_rotmat_Delta,
                                   std::vector<COMPLEX> &G) {
  assert(FLAVORS == SITES * SPINS);
  assert(G.size() == FLAVORS * FLAVORS * Np1);
  assert(SPINS == 2);

  MAT_COMPLEX mattmp(FLAVORS, FLAVORS), mattmp2(FLAVORS, FLAVORS);
  for (int time = 0; time < Np1; ++time) {
    for (int iflavor = 0; iflavor < FLAVORS; ++iflavor) {
      for (int jflavor = 0; jflavor < FLAVORS; ++jflavor) {
        mattmp(iflavor, jflavor) = G[(iflavor * FLAVORS + jflavor) * Np1 + time];
      }
    }
    mattmp2 = rotmat_Delta * mattmp * inv_rotmat_Delta;
    for (int iflavor = 0; iflavor < FLAVORS; ++iflavor) {
      for (int jflavor = 0; jflavor < FLAVORS; ++jflavor) {
        G[(iflavor * FLAVORS + jflavor) * Np1 + time] = mattmp2(iflavor, jflavor);
      }
    }
  }
}

template<typename IMP_MODEL>
void HybridizationSimulation<IMP_MODEL>::sanity_check() {
#ifndef NDEBUG
  mc_config.check_nan();
  mc_config.sanity_check(sliding_window);
#endif
}


template<typename IMP_MODEL>
void HybridizationSimulation<IMP_MODEL>::adjust_worm_space_weight() {
  if (!p_flat_histogram_config_space || is_thermalized()) return;

  //measure current configuration space
  //if (mc_config.current_config_space() == Z_FUNCTION) {
  //p_flat_histogram_config_space->measure(0);
  //} else {
  //const int i_worm = get_worm_position(mc_config.current_config_space());
  //p_flat_histogram_config_space->measure(i_worm + 1);
  //}
  p_flat_histogram_config_space->measure(get_config_space_position(mc_config.current_config_space()));

  // If the histogram is flat enough, make the modification factor smaller
  if (p_flat_histogram_config_space->flat_enough()) {
    p_flat_histogram_config_space->update_lambda(false);
  }

  // Apply the new worm space weights
  config_space_extra_weight[0] = 1.0;
  for (int w = 0; w < worm_types.size(); ++w) {
    config_space_extra_weight[w + 1] = p_flat_histogram_config_space->weight_ratio(w + 1, 0);
    worm_space_extra_weight_map[worm_types[w]] = p_flat_histogram_config_space->weight_ratio(w + 1, 0);
  }
}
