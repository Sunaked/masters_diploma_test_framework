-- ============================================================================
-- prod.lua — production benchmark для магистерской защиты.
--
-- Методология:
--   1. Каждый сценарий исследует КОНКРЕТНЫЙ аспект реализации контейнера
--   2. Prefill выводит контейнер в steady-state ДО замера
--   3. Warmup снимает cold-start артефакты
--   4. 7 runs × CI95 — достаточно для t-теста между контейнерами
--   5. Шкала потоков 1→16 показывает масштабируемость
--
-- Что ожидаем увидеть на защите:
--   - ShardedHashMap: деградация p999 при hotspot (shard convoy)
--   - lock-free (CDS/CK): лучший scaling, но allocator pressure при write-heavy
--   - std::unordered_map+mutex: baseline с катастрофическим scaling
--
-- Время: ~15-25 минут на один контейнер при данных параметрах.
-- ============================================================================

global = {
	target_total_time_minutes = 120,
	base_runs_per_config = 7, -- 7 runs → df=6 → t_crit=2.447
	min_runs_per_config = 7, -- для стабильного CI95
	metrics_sampling_ms = 100,
	pin_threads = true,
	use_rdtsc_for_latency = true,
	latency_off = false,
	sampler_off = false,
	latency_mode = "both",
	reservoir_capacity = 1000000,
	warmup_auto_seconds = 0.0,
}

-- Шкала потоков: от 1 до ядер; 1 поток — baseline, далее степени двойки
threads_list = { 1, 2, 4, 8, 16 }

containers = {
	{
		name = "ShardedHashMap256",
		type = "custom",
		target_time_minutes = 30,
	},
	-- Раскомментировать когда адаптеры подключены:
	-- {
	--     name = "CDS_SplitListMap",
	--     type = "cds_split",
	--     target_time_minutes = 30,
	-- },
	-- {
	--     name = "CDS_FeldmanHashMap",
	--     type = "cds_feldman",
	--     target_time_minutes = 30,
	-- },
	-- {
	--     name = "CK_HT",
	--     type = "ck_ht",
	--     target_time_minutes = 30,
	-- },
}

-- ============================================================================
-- Lua generators
-- ============================================================================

-- Один горячий ключ: 50% find по key=0, остальное — вставки/удаления
-- по широкому диапазону. Максимальная contention на одном shard/bucket.
function single_hotkey(ctx, i)
	if i % 2 == 0 then
		return { op = "find", key = 0 }
	elseif i % 7 == 0 then
		return { op = "erase", key = (i * 2654435761) % ctx.key_range }
	else
		return { op = "insert", key = (i * 2654435761) % ctx.key_range + ctx.thread_id * ctx.key_range }
	end
end

-- Группа из 16 горячих ключей: имитирует реальный hotspot (session cache, user profile).
-- 70% find горячих, 20% find холодных, 10% write.
function narrow_hotspot_16(ctx, i)
	local hot_key = i % 16
	local r = i % 10
	if r < 7 then
		return { op = "find", key = hot_key }
	elseif r < 9 then
		return { op = "find", key = (i * 2654435761) % ctx.key_range }
	elseif r % 2 == 0 then
		return { op = "insert", key = (i * 2654435761) % ctx.key_range + ctx.thread_id * ctx.key_range }
	else
		return { op = "erase", key = (i * 2654435761) % ctx.key_range }
	end
end

-- Read-modify-write: find → условный insert/update. Паттерн кэша.
function read_modify_write(ctx, i)
	local key = (i * 2654435761) % ctx.key_range
	if i % 3 == 0 then
		return { op = "find", key = key }
	elseif i % 3 == 1 then
		return { op = "insert", key = key }
	else
		return { op = "find", key = key }
	end
end

-- ============================================================================
-- Сценарии
-- ============================================================================

scenarios = {

	-- ==================================================================
	-- A. READ-DOMINANT STEADY STATE (YCSB-B аналог)
	--
	-- Что показывает:
	--   - Базовый throughput/latency в типичном read-heavy режиме
	--   - Overhead reader-writer lock (shared_lock) vs lock-free read
	--   - Scaling читателей (идеальный: линейный)
	--
	-- Ожидание:
	--   - lock-free: ~линейный scaling до числа ядер
	--   - ShardedHashMap: хороший scaling (shared_lock дешёвый)
	--   - global mutex: нулевой scaling
	-- ==================================================================
	{
		name = "A_read_dominant_95r",
		phases = {
			{
				mode = "scripted_plan",
				role = "prefill",
				plan = {
					{ count = 1000000, op = "insert", key_mode = "sequential", start = 0 },
				},
			},
			{
				mode = "probabilistic",
				role = "warmup",
				ops_per_thread = 500000,
				insert = 0.05,
				find = 0.90,
				erase = 0.05,
				key_dist = "uniform",
				key_range = 1000000,
			},
			{
				mode = "probabilistic",
				role = "measure",
				ops_per_thread = 5000000,
				insert = 0.02,
				find = 0.95,
				erase = 0.03,
				key_dist = "uniform",
				key_range = 1000000,
			},
		},
	},

	-- ==================================================================
	-- B. WRITE-HEAVY (YCSB-A аналог)
	--
	-- Что показывает:
	--   - Contention на exclusive locks / CAS loops
	--   - Allocator pressure (частые insert/erase = malloc/free)
	--   - Scaling под write load
	--
	-- Ожидание:
	--   - ShardedHashMap: деградация из-за unique_lock contention
	--   - lock-free: CAS retry storms при высокой contention
	--   - p999 взрывается у всех, но по-разному
	-- ==================================================================
	{
		name = "B_write_heavy_50w",
		phases = {
			{
				mode = "scripted_plan",
				role = "prefill",
				plan = {
					{ count = 500000, op = "insert", key_mode = "sequential", start = 0 },
				},
			},
			{
				mode = "probabilistic",
				role = "warmup",
				ops_per_thread = 300000,
				insert = 0.25,
				find = 0.50,
				erase = 0.25,
				key_dist = "uniform",
				key_range = 500000,
			},
			{
				mode = "probabilistic",
				role = "measure",
				ops_per_thread = 3000000,
				insert = 0.25,
				find = 0.50,
				erase = 0.25,
				key_dist = "uniform",
				key_range = 500000,
			},
		},
	},

	-- ==================================================================
	-- C. ZIPFIAN SKEW (YCSB-E/реалистичный доступ)
	--
	-- Что показывает:
	--   - Поведение при неравномерном доступе (alpha=0.99 → top-1% ключей
	--     получают ~50% запросов)
	--   - Shard imbalance в ShardedHashMap
	--   - Эффективность cache locality (горячие ключи в L1/L2)
	--
	-- Ожидание:
	--   - Все контейнеры: p50 лучше чем uniform (cache hits)
	--   - ShardedHashMap: p999 хуже (горячие шарды перегружены)
	--   - lock-free: меньше деградация хвоста
	-- ==================================================================
	{
		name = "C_zipfian_skew_099",
		phases = {
			{
				mode = "scripted_plan",
				role = "prefill",
				plan = {
					{ count = 1000000, op = "insert", key_mode = "sequential", start = 0 },
				},
			},
			{
				mode = "probabilistic",
				role = "warmup",
				ops_per_thread = 500000,
				insert = 0.05,
				find = 0.90,
				erase = 0.05,
				key_dist = "zipfian",
				alpha = 0.99,
				key_range = 1000000,
			},
			{
				mode = "probabilistic",
				role = "measure",
				ops_per_thread = 5000000,
				insert = 0.05,
				find = 0.90,
				erase = 0.05,
				key_dist = "zipfian",
				alpha = 0.99,
				key_range = 1000000,
			},
		},
	},

	-- ==================================================================
	-- D. SINGLE HOT KEY CONTENTION (worst case)
	--
	-- Что показывает:
	--   - Абсолютный worst-case: ВСЕ потоки стучат в ОДНУ точку
	--   - Shard convoy (ShardedHashMap: один shard под unique_lock)
	--   - CAS contention (lock-free: retry loop)
	--
	-- Ожидание:
	--   - ShardedHashMap: throughput ПАДАЕТ с ростом потоков
	--   - lock-free: throughput стагнирует, но не падает
	--   - Это самый показательный слайд для защиты
	-- ==================================================================
	{
		name = "D_single_hotkey_contention",
		phases = {
			{
				mode = "scripted_plan",
				role = "prefill",
				plan = {
					{ count = 200000, op = "insert", key_mode = "sequential", start = 0 },
				},
			},
			{
				mode = "scripted_step",
				role = "warmup",
				ops_per_thread = 200000,
				script = "single_hotkey",
				key_range = 200000,
			},
			{
				mode = "scripted_step",
				role = "measure",
				ops_per_thread = 2000000,
				script = "single_hotkey",
				key_range = 200000,
			},
		},
	},

	-- ==================================================================
	-- E. NARROW HOTSPOT 16 KEYS (реалистичный hotspot)
	--
	-- Что показывает:
	--   - 16 горячих ключей = 16 / 256 шардов = 6.25% шардов горячие
	--   - При 4+ потоках: столкновения на горячих шардах
	--   - Сравнение с D: насколько расширение hotspot помогает
	--
	-- Ожидание:
	--   - ShardedHashMap: значительно лучше чем D (16 шардов vs 1)
	--   - lock-free: одинаково хорош в D и E
	--   - Разница D vs E = цена contention mitigation
	-- ==================================================================
	{
		name = "E_narrow_hotspot_16keys",
		phases = {
			{
				mode = "scripted_plan",
				role = "prefill",
				plan = {
					{ count = 500000, op = "insert", key_mode = "sequential", start = 0 },
				},
			},
			{
				mode = "scripted_step",
				role = "warmup",
				ops_per_thread = 300000,
				script = "narrow_hotspot_16",
				key_range = 500000,
			},
			{
				mode = "scripted_step",
				role = "measure",
				ops_per_thread = 3000000,
				script = "narrow_hotspot_16",
				key_range = 500000,
			},
		},
	},

	-- ==================================================================
	-- F. GROWING WORKING SET (insert-only burst)
	--
	-- Что показывает:
	--   - Поведение при монотонном росте структуры
	--   - Rehash / resize events (std::unordered_map)
	--   - Allocator scalability (malloc contention)
	--   - RSS growth rate
	--
	-- Ожидание:
	--   - std::unordered_map: spikes в latency при rehash
	--   - ShardedHashMap: мягче (256 независимых rehash)
	--   - lock-free: зависит от allocator (jemalloc vs glibc)
	-- ==================================================================
	{
		name = "F_growing_working_set",
		phases = {
			-- Нет prefill — начинаем с пустого контейнера
			{
				mode = "probabilistic",
				role = "warmup",
				ops_per_thread = 50000,
				insert = 1.0,
				find = 0.0,
				erase = 0.0,
				key_dist = "sequential",
				key_range = 10000000,
			},
			-- Основной замер: чистый insert sequential
			{
				mode = "scripted_plan",
				role = "measure",
				plan = {
					{ count = 3000000, op = "insert", key_mode = "sequential", start = 100000 },
				},
			},
		},
	},

	-- ==================================================================
	-- G. POST-DELETE FRAGMENTATION
	--
	-- Что показывает:
	--   - Вставляем, потом удаляем ~70%, потом снова читаем/пишем
	--   - Tombstones / fragmentation / retained capacity
	--   - Деградация find после массового delete
	--
	-- Ожидание:
	--   - open addressing: probe chain degradation (tombstones)
	--   - chaining (ShardedHashMap): меньше проблем
	--   - lock-free: зависит от epoch-based reclamation
	-- ==================================================================
	{
		name = "G_post_delete_fragmentation",
		phases = {
			-- Заполняем 1M ключей
			{
				mode = "scripted_plan",
				role = "prefill",
				plan = {
					{ count = 1000000, op = "insert", key_mode = "sequential", start = 0 },
				},
			},
			-- Удаляем 70% (sequential, первые 700K)
			{
				mode = "scripted_plan",
				role = "prefill",
				plan = {
					{ count = 700000, op = "erase", key_mode = "sequential", start = 0 },
				},
			},
			-- Warmup на разреженной структуре
			{
				mode = "probabilistic",
				role = "warmup",
				ops_per_thread = 300000,
				insert = 0.10,
				find = 0.80,
				erase = 0.10,
				key_dist = "uniform",
				key_range = 1000000,
			},
			-- Замер: read-heavy по разреженной структуре
			{
				mode = "probabilistic",
				role = "measure",
				ops_per_thread = 3000000,
				insert = 0.10,
				find = 0.80,
				erase = 0.10,
				key_dist = "uniform",
				key_range = 1000000,
			},
		},
	},

	-- ==================================================================
	-- H. READ-MODIFY-WRITE PATTERN (cache/session store)
	--
	-- Что показывает:
	--   - Паттерн: find → insert (update) → find (verify)
	--   - Чередование shared_lock → unique_lock на одних ключах
	--   - Lock upgrade / downgrade cost
	--
	-- Ожидание:
	--   - ShardedHashMap: хороший p50, но p99 выше из-за lock switching
	--   - lock-free: стабильный latency профиль
	-- ==================================================================
	{
		name = "H_read_modify_write",
		phases = {
			{
				mode = "scripted_plan",
				role = "prefill",
				plan = {
					{ count = 500000, op = "insert", key_mode = "sequential", start = 0 },
				},
			},
			{
				mode = "scripted_step",
				role = "warmup",
				ops_per_thread = 300000,
				script = "read_modify_write",
				key_range = 500000,
			},
			{
				mode = "scripted_step",
				role = "measure",
				ops_per_thread = 3000000,
				script = "read_modify_write",
				key_range = 500000,
			},
		},
	},
}

-- ============================================================================
-- Матрица покрытия (для слайда "Методология"):
--
--  Сценарий  | Read% | Write% | KeyDist   | Hotspot | Что атакует
-- -----------|-------|--------|-----------|---------|---------------------------
--  A         |  95   |   5    | uniform   | нет     | reader scaling
--  B         |  50   |  50    | uniform   | нет     | write contention
--  C         |  90   |  10    | zipfian   | мягкий  | shard imbalance
--  D         |  50   |  50    | scripted  | 1 ключ  | worst-case contention
--  E         |  70   |  30    | scripted  | 16 кл.  | реалистичный hotspot
--  F         |   0   | 100    | sequential| нет     | resize/allocator
--  G         |  80   |  20    | uniform   | нет     | fragmentation
--  H         |  67   |  33    | scripted  | нет     | lock switching cost
--
-- Шкала потоков: 1, 2, 4, 8, 16
-- Runs per config: 7 (CI95 с df=6)
-- Итого: 8 сценариев × 5 threads × 7 runs = 280 прогонов на контейнер
-- ============================================================================
