-- ============================================================================
-- smoke.lua — быстрый smoke test всех возможностей фреймворка
--
-- Цель: за < 30 секунд проверить, что ВСЕ code paths работают:
--   - Оба типа контейнеров (custom + null)
--   - Все PhaseMode (probabilistic, scripted_step, scripted_plan)
--   - Все PhaseRole (prefill, warmup, measure, cooldown)
--   - Все KeyDist (uniform, zipfian, sequential)
--   - Все PlanKeyMode (sequential, fixed, uniform, zipfian)
--   - Все LatencyMode (both)
--   - Reservoir sampling
--   - Per-phase results
--   - SystemSampler
--   - Multi-thread
--
-- Операций мало — это НЕ performance test, а structural validation.
-- ============================================================================

global = {
	target_total_time_minutes = 1,
	base_runs_per_config = 2,
	min_runs_per_config = 2,
	metrics_sampling_ms = 50,
	pin_threads = true,
	use_rdtsc_for_latency = true,

	latency_off = false,
	sampler_off = false,
	latency_mode = "both",
	reservoir_capacity = 10000,
	warmup_auto_seconds = 0.0,
}

threads_list = { 1, 2 }

containers = {
	{
		name = "SmokeCustContainer",
		type = "custom",
		target_time_minutes = 1,
	},
	{
		name = "SmokeNullBaseline",
		type = "null",
		target_time_minutes = 1,
	},
}

-- ============================================================================
-- Lua generators (маленькие, но покрывают все паттерны)
-- ============================================================================

-- Классический alternating hotspot
function smoke_hotspot(ctx, i)
	if i % 7 == 0 then
		return { op = "erase", key = i % 200 }
	elseif i % 3 == 0 then
		return { op = "find", key = 42 }
	else
		return { op = "insert", key = i + ctx.thread_id * ctx.ops_per_thread }
	end
end

-- Multi-return shorthand (проверяем оба формата возврата)
function smoke_burst(ctx, i)
	local cycle = i % 10
	if cycle < 6 then
		return "insert", i + ctx.thread_id * 100000
	elseif cycle < 9 then
		return "find", (i % 500)
	else
		return "erase", (i % 300)
	end
end

-- Тяжёлый sequential scan → read
function smoke_scan(ctx, i)
	local switch = math.floor(ctx.ops_per_thread * 0.6)
	local base = ctx.thread_id * ctx.ops_per_thread
	if i <= switch then
		return { op = "insert", key = base + i }
	else
		return { op = "find", key = base + ((i - switch) % switch) + 1 }
	end
end

-- ============================================================================
-- Сценарии
-- ============================================================================

scenarios = {

	-- ==================================================================
	-- 1. Полный pipeline: prefill → warmup → measure → cooldown
	--    Проверяет все 4 роли + scripted_plan + probabilistic + scripted_step
	-- ==================================================================
	{
		name = "full_pipeline",
		phases = {
			-- prefill: sequential inserts (вне замера)
			{
				mode = "scripted_plan",
				role = "prefill",
				plan = {
					{ count = 5000, op = "insert", key_mode = "sequential", start = 0 },
				},
			},
			-- warmup: probabilistic zipfian (stats отбрасываются)
			{
				mode = "probabilistic",
				role = "warmup",
				ops_per_thread = 2000,
				insert = 0.1,
				find = 0.8,
				erase = 0.1,
				key_dist = "zipfian",
				alpha = 0.99,
				key_range = 5000,
			},
			-- measure: probabilistic uniform
			{
				mode = "probabilistic",
				role = "measure",
				ops_per_thread = 10000,
				insert = 0.2,
				find = 0.7,
				erase = 0.1,
				key_dist = "uniform",
				key_range = 5000,
			},
			-- measure: scripted_step hotspot
			{
				mode = "scripted_step",
				role = "measure",
				ops_per_thread = 5000,
				script = "smoke_hotspot",
				key_range = 5000,
			},
			-- cooldown: light erase (stats отбрасываются)
			{
				mode = "scripted_plan",
				role = "cooldown",
				plan = {
					{ count = 1000, op = "erase", key_mode = "sequential", start = 0 },
				},
			},
		},
	},

	-- ==================================================================
	-- 2. Только probabilistic: все три key_dist
	-- ==================================================================
	{
		name = "prob_all_dists",
		phases = {
			-- prefill чтобы find/erase не были пустыми
			{
				mode = "scripted_plan",
				role = "prefill",
				plan = {
					{ count = 3000, op = "insert", key_mode = "sequential", start = 0 },
				},
			},
			-- uniform
			{
				mode = "probabilistic",
				role = "measure",
				ops_per_thread = 5000,
				insert = 0.1,
				find = 0.8,
				erase = 0.1,
				key_dist = "uniform",
				key_range = 3000,
			},
			-- zipfian
			{
				mode = "probabilistic",
				role = "measure",
				ops_per_thread = 5000,
				insert = 0.3,
				find = 0.5,
				erase = 0.2,
				key_dist = "zipfian",
				alpha = 0.99,
				key_range = 3000,
			},
			-- sequential (insert-heavy)
			{
				mode = "probabilistic",
				role = "measure",
				ops_per_thread = 3000,
				insert = 0.8,
				find = 0.15,
				erase = 0.05,
				key_dist = "sequential",
				key_range = 100000,
			},
		},
	},

	-- ==================================================================
	-- 3. ScriptedPlan: все PlanKeyMode в одном плане
	-- ==================================================================
	{
		name = "plan_all_keymodes",
		phases = {
			{
				mode = "scripted_plan",
				role = "prefill",
				plan = {
					{ count = 2000, op = "insert", key_mode = "sequential", start = 0 },
				},
			},
			{
				mode = "scripted_plan",
				role = "measure",
				plan = {
					-- sequential inserts
					{ count = 1000, op = "insert", key_mode = "sequential", start = 2000 },
					-- fixed-key hotspot finds
					{ count = 2000, op = "find", key_mode = "fixed", fixed_key = 500 },
					-- uniform range finds
					{ count = 2000, op = "find", key_mode = "uniform", key_range = 3000 },
					-- zipfian erases
					{ count = 1000, op = "erase", key_mode = "zipfian", key_range = 3000, alpha = 0.8 },
				},
			},
		},
	},

	-- ==================================================================
	-- 4. ScriptedStep: все три Lua генератора
	-- ==================================================================
	{
		name = "scripted_all_generators",
		phases = {
			{
				mode = "scripted_plan",
				role = "prefill",
				plan = {
					{ count = 3000, op = "insert", key_mode = "sequential", start = 0 },
				},
			},
			{
				mode = "scripted_step",
				role = "measure",
				ops_per_thread = 4000,
				script = "smoke_hotspot",
				key_range = 3000,
			},
			{
				mode = "scripted_step",
				role = "measure",
				ops_per_thread = 4000,
				script = "smoke_burst",
				key_range = 3000,
			},
			{
				mode = "scripted_step",
				role = "measure",
				ops_per_thread = 3000,
				script = "smoke_scan",
				key_range = 3000,
			},
		},
	},

	-- ==================================================================
	-- 5. Минимальный: одна фаза, measure-only, без prefill/warmup
	--    Проверяет backward compatibility (нет role = default measure)
	-- ==================================================================
	{
		name = "minimal_single_phase",
		phases = {
			{
				mode = "probabilistic",
				-- role не указан → default "measure"
				ops_per_thread = 8000,
				insert = 0.33,
				find = 0.34,
				erase = 0.33,
				key_dist = "uniform",
				key_range = 1000,
			},
		},
	},

	-- ==================================================================
	-- 6. Warmup-only + measure: проверяет что warmup stats отбрасываются
	-- ==================================================================
	{
		name = "warmup_then_measure",
		phases = {
			{
				mode = "probabilistic",
				role = "warmup",
				ops_per_thread = 5000,
				insert = 0.5,
				find = 0.5,
				erase = 0.0,
				key_dist = "uniform",
				key_range = 2000,
			},
			{
				mode = "probabilistic",
				role = "measure",
				ops_per_thread = 10000,
				insert = 0.1,
				find = 0.8,
				erase = 0.1,
				key_dist = "zipfian",
				alpha = 0.5,
				key_range = 2000,
			},
		},
	},

	-- ==================================================================
	-- 7. Heavy write → heavy read: фазовый переход
	-- ==================================================================
	{
		name = "write_to_read_transition",
		phases = {
			{
				mode = "scripted_plan",
				role = "measure",
				plan = {
					{ count = 5000, op = "insert", key_mode = "sequential", start = 0 },
				},
			},
			{
				mode = "scripted_plan",
				role = "measure",
				plan = {
					{ count = 8000, op = "find", key_mode = "uniform", key_range = 5000 },
				},
			},
		},
	},
}
