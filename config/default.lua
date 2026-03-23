-- ============================================================================
-- default.lua — конфигурация бенчмарка с тремя режимами генерации trace.
--
-- Режимы фаз:
--   "probabilistic"  — YCSB-стиль (вектор вероятностей + распределение ключей)
--   "scripted_step"  — Lua callback per operation: gen(ctx, i) → {op, key}
--   "scripted_plan"  — декларативный план: [{count, op, key_mode, ...}]
--
-- ИНВАРИАНТ: Lua вызывается ТОЛЬКО при прегенерации trace.
-- Горячий цикл бенчмарка НЕ обращается к Lua.
-- ============================================================================

global = {
	target_total_time_minutes = 5,
	base_runs_per_config = 5,
	min_runs_per_config = 3,
	metrics_sampling_ms = 200,
	pin_threads = true,
	use_rdtsc_for_latency = true,
}

--threads_list = {1, 2, 4, 8, 16, 24, 32, 48, 64}
threads_list = { 1, 4 }

containers = {
	{
		name = "MyOwnContainer",
		type = "custom",
		target_time_minutes = 5,
	},
}

-- ============================================================================
-- Lua generator functions для ScriptedStep фаз.
-- Контракт:  function(ctx, i) → { op = "...", key = N }
-- Или:       function(ctx, i) → "op_string", key_number
--
-- ctx содержит:
--   ctx.scenario_name, ctx.phase_index, ctx.thread_id,
--   ctx.thread_count, ctx.ops_per_thread, ctx.seed,
--   ctx.key_range, ctx.alpha
--
-- i = 1..ops_per_thread (Lua-style, от 1)
--
-- ВАЖНО: функция должна быть детерминированной при фиксированном seed.
-- ============================================================================

-- Пример 1: alternating hotspot
-- Чётные i → find по горячему ключу 42
-- Каждый 10-й → erase
-- Остальные → insert последовательных ключей
function alternating_hotspot(ctx, i)
	if i % 10 == 0 then
		return { op = "erase", key = i % 1000 }
	elseif i % 2 == 0 then
		return { op = "find", key = 42 }
	else
		return { op = "insert", key = i + ctx.thread_id * ctx.ops_per_thread }
	end
end

-- Пример 2: scan-like pattern
-- Сначала 70% insert (последовательные ключи), затем find по тем же ключам.
-- Переключение на find происходит на 70% от ops_per_thread.
function scan_then_read(ctx, i)
	local switch_point = math.floor(ctx.ops_per_thread * 0.7)
	local base_key = ctx.thread_id * ctx.ops_per_thread

	if i <= switch_point then
		return { op = "insert", key = base_key + i }
	else
		-- Читаем ранее вставленные ключи (циклически)
		local read_key = base_key + ((i - switch_point) % switch_point) + 1
		return { op = "find", key = read_key }
	end
end

-- Пример 3: burst pattern с multi-return (shorthand)
-- Быстрые вставки пачками по 100, потом 50 find, потом 10 erase, и так далее.
function burst_pattern(ctx, i)
	local cycle = i % 160 -- 100 + 50 + 10

	if cycle < 100 then
		return "insert", i + ctx.thread_id * 1000000
	elseif cycle < 150 then
		return "find", (i % 10000)
	else
		return "erase", (i % 5000)
	end
end

-- ============================================================================
-- Сценарии
-- ============================================================================

scenarios = {
	-- ====================================================================
	-- 1. Probabilistic: классический YCSB-стиль
	-- ====================================================================
	{
		name = "steady_zipfian_70r_20i_10d",
		phases = {
			{
				mode = "probabilistic", -- можно опустить, это default
				ops_per_thread = 100000,
				insert = 0.20,
				find = 0.70,
				erase = 0.10,
				key_dist = "zipfian",
				alpha = 0.99,
				key_range = 10000000,
			},
		},
	},

	-- ====================================================================
	-- 2. ScriptedStep: Lua callback per operation
	-- ====================================================================
	{
		name = "alternating_hotspot_scripted",
		phases = {
			{
				mode = "scripted_step",
				ops_per_thread = 50000,
				script = "alternating_hotspot", -- имя Lua-функции выше
				key_range = 1000000, -- передаётся в ctx
			},
		},
	},

	{
		name = "scan_then_read_scripted",
		phases = {
			{
				mode = "scripted_step",
				ops_per_thread = 100000,
				script = "scan_then_read",
				key_range = 10000000,
			},
		},
	},

	{
		name = "burst_pattern_scripted",
		phases = {
			{
				mode = "scripted_step",
				ops_per_thread = 80000,
				script = "burst_pattern",
				key_range = 1000000,
			},
		},
	},

	-- ====================================================================
	-- 3. ScriptedPlan: декларативный план (без Lua-вызовов при генерации)
	-- ====================================================================
	{
		name = "prefill_then_hotspot_plan",
		phases = {
			{
				mode = "scripted_plan",
				plan = {
					-- Фаза 1: последовательная вставка 50K ключей (prefill)
					{ count = 50000, op = "insert", key_mode = "sequential", start = 0 },

					-- Фаза 2: горячий find по одному ключу (hotspot)
					{ count = 100000, op = "find", key_mode = "fixed", fixed_key = 42 },

					-- Фаза 3: удаление по Zipfian
					{ count = 30000, op = "erase", key_mode = "zipfian", key_range = 50000, alpha = 0.99 },
				},
			},
		},
	},

	{
		name = "mixed_plan_uniform",
		phases = {
			{
				mode = "scripted_plan",
				plan = {
					-- Bulk insert
					{ count = 100000, op = "insert", key_mode = "uniform", key_range = 10000000 },

					-- Read-heavy phase
					{ count = 200000, op = "find", key_mode = "uniform", key_range = 10000000 },

					-- Cleanup
					{ count = 50000, op = "erase", key_mode = "uniform", key_range = 10000000 },
				},
			},
		},
	},

	-- ====================================================================
	-- 4. Многофазный сценарий: микс режимов в одном сценарии
	-- ====================================================================
	{
		name = "multi_phase_mixed",
		phases = {
			-- Фаза 1: prefill через ScriptedPlan
			{
				mode = "scripted_plan",
				plan = {
					{ count = 1000000, op = "insert", key_mode = "sequential", start = 0 },
				},
			},
			-- Фаза 2: read-heavy probabilistic
			{
				mode = "probabilistic",
				ops_per_thread = 2000000,
				insert = 0.05,
				find = 0.90,
				erase = 0.05,
				key_dist = "zipfian",
				alpha = 0.99,
				key_range = 100000,
			},
			-- Фаза 3: scripted cleanup
			{
				mode = "scripted_step",
				ops_per_thread = 500000,
				script = "alternating_hotspot",
				key_range = 100000,
			},
		},
	},
}
