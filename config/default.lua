-- ============================================================================
-- default.lua — конфигурация бенчмарка
--
-- Все параметры считываются до старта замеров. Lua-интерпретатор НЕ вызывается
-- во время выполнения горячего цикла (нулевые накладные расходы на конфигурацию).
-- ============================================================================

global = {
	target_total_time_minutes = 5, -- желаемое общее время эксперимента (минуты)
	base_runs_per_config = 5, -- базовое кол-во повторений на каждую пару (threads × scenario)
	min_runs_per_config = 3, -- гарантия минимального числа повторений
	metrics_sampling_ms = 200, -- частота системных метрик (мс)
	pin_threads = true, -- CPU affinity (sched_setaffinity)
	use_rdtsc_for_latency = true, -- rdtsc vs std::chrono для latency
	ops_per_ms_per_thread = 500,
}

--threads_list = { 1, 2, 4, 8, 16, 24, 32, 48, 64 }
threads_list = { 1, 2 }

containers = {
	--{
	--    name = "SplitListMap",
	--    type = "cds::SplitListMap",
	--    target_time_minutes = 90,
	--},
	--{
	--    name = "FeldmanHashMap",
	--    type = "cds::FeldmanHashMap",
	--    target_time_minutes = 60,
	--},
	--{
	--    name = "CK_HT",
	--    type = "ck_ht",
	--    target_time_minutes = 50,
	--},
	{
		name = "MyOwnContainer",
		type = "custom",
		target_time_minutes = 5, -- (минуты)
	},
}

scenarios = {
	{
		name = "steady_zipfian_70r_20i_10d",
		phases = {
			{
				duration_ms = 15000,
				insert = 0.20,
				find = 0.70,
				erase = 0.10,
				key_dist = "zipfian",
				alpha = 0.99,
				key_range = 10000000,
			},
		},
	},
	{
		name = "cache_bust_then_hotspot",
		phases = {
			{
				duration_ms = 8000,
				insert = 1.00,
				find = 0.00,
				erase = 0.00,
				key_dist = "uniform",
				key_range = 1000000000,
			},
			{
				duration_ms = 12000,
				insert = 0.00,
				find = 1.00,
				erase = 0.00,
				key_dist = "zipfian",
				alpha = 0.99,
				key_range = 1000000,
			},
			{
				duration_ms = 6000,
				insert = 0.00,
				find = 0.00,
				erase = 1.00,
				key_dist = "zipfian",
				alpha = 0.99,
				key_range = 1000000,
			},
		},
	},
}
