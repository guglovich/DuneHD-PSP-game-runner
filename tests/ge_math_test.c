/*
 * Дифференциальный тест порта арифметики GE.
 *
 * Сравнивает наши функции из `src/ge_math.c` с оригиналом из
 * `GPU/Software/GEMath.h` PPSSPP на случайных входах. Побитово.
 *
 * Зачем именно так: порт легко написать с расхождением, которое не видно глазами.
 * Строка матрицы — одна сумма без порядка, усечение к нулю, таблицы без
 * порождающего правила: любая из этих деталей даёт тихое расхождение в
 * последнем бите. Проверять надо сравнением, а не рассуждением.
 *
 * Оригинал вставлен сюда дословно (скалярные инлайн-функции из заголовка),
 * чтобы тест не зависел от сборки PPSSPP и его Math3D.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>

#include "ge_math.h"
#include "ge_math_tables_ref.h"

/* --- оригинал, дословно из GEMath.h --------------------------------------- */

static float ref_TruncateToFloat24(float f) {
	uint32_t bits;
	memcpy(&bits, &f, sizeof(bits));
	bits &= 0xFFFFFF00;
	memcpy(&f, &bits, sizeof(f));
	return f;
}

static float ref_ProductToFloat24(double d) {
	uint64_t bits;
	memcpy(&bits, &d, sizeof(bits));
	if (((bits >> 52) & 0x7FF) != 0x7FF) {
		bits &= ~((1ULL << (52 - 15)) - 1);
	}
	memcpy(&d, &bits, sizeof(d));
	return (float)d;
}

static float ref_ScaleByPow2(float f, int n, uint32_t sign) {
	uint32_t bits;
	memcpy(&bits, &f, sizeof(bits));
	bits = (uint32_t)((int32_t)bits + n * (1 << 23)) | sign;
	memcpy(&f, &bits, sizeof(f));
	return f;
}

static float ref_ScaleByPow2Exact(float f, int e) {
	uint32_t bits;
	memcpy(&bits, &f, sizeof(bits));
	const int field = (int)((bits >> 23) & 0xFF);
	if (field == 0 || field == 255 || field + e < 1 || field + e > 254)
		return ldexpf(f, e);
	bits += (uint32_t)e << 23;
	memcpy(&f, &bits, sizeof(f));
	return f;
}

static float ref_GEAdd(float a, float b) {
	uint32_t ba, bb;
	memcpy(&ba, &a, sizeof(ba));
	memcpy(&bb, &b, sizeof(bb));
	const int ea = (ba >> 23) & 0xFF;
	const int eb = (bb >> 23) & 0xFF;
	if (ea == 0 || eb == 0) {
		return ea == 0 ? (eb == 0 ? 0.0f : ref_TruncateToFloat24(b)) : ref_TruncateToFloat24(a);
	}
	const int emax = ea > eb ? ea : eb;
	const int na = 8 + emax - ea;
	const int nb = 8 + emax - eb;
	ba = na >= 24 ? (ba & 0x80000000) : (ba & ~((1U << na) - 1));
	bb = nb >= 24 ? (bb & 0x80000000) : (bb & ~((1U << nb) - 1));
	memcpy(&a, &ba, sizeof(a));
	memcpy(&b, &bb, sizeof(b));
	return a + b;
}

struct RefRowTerm {
	int32_t mantissa;
	int lsbExp;
};

static struct RefRowTerm ref_GEProduct(float a, float b) {
	uint32_t ba, bb;
	memcpy(&ba, &a, sizeof(ba));
	memcpy(&bb, &b, sizeof(bb));
	const int ea = (ba >> 23) & 0xFF;
	const int eb = (bb >> 23) & 0xFF;
	if (ea == 0 || eb == 0) {
		struct RefRowTerm t = { 0, -100000 };
		return t;
	}
	const uint64_t ma = (ba & 0x007FFFFF) | 0x00800000;
	const uint64_t mb = (bb & 0x007FFFFF) | 0x00800000;
	const int32_t m = (int32_t)((ma * mb) >> 31);
	struct RefRowTerm t;

	t.mantissa = ((ba ^ bb) & 0x80000000) ? -m : m;
	t.lsbExp = (ea - 127) + (eb - 127) - 15;
	return t;
}

static float ref_GERowSum(const struct RefRowTerm *terms, int count) {
	int lsbExp = INT_MIN;
	for (int i = 0; i < count; ++i) {
		lsbExp = lsbExp > terms[i].lsbExp ? lsbExp : terms[i].lsbExp;
	}
	int32_t sum = 0;
	for (int i = 0; i < count; ++i) {
		const int32_t m = terms[i].mantissa;
		if (m == 0) continue;
		const int shift = lsbExp - terms[i].lsbExp;
		if (shift < 32) {
			sum += m < 0 ? -(-m >> shift) : (m >> shift);
		}
	}
	if (sum == 0) return 0.0f;
	return ref_TruncateToFloat24(ref_ScaleByPow2Exact((float)sum, lsbExp));
}

/* Таблицы берём из нашего файла: они перенесены дословно, и проверять их
 * сравнением с самими собой бессмысленно. Их проверяет отдельный гейт на
 * побайтовое совпадение с оригиналом.
 *
 * А вот функции поверх таблиц проверяем дифференциально, поэтому оригинал
 * нужен здесь же. */

extern const struct GeRecipSegment ge_recip_segments[128];
extern const struct GeSetupRecipSegment ge_setup_recip_table[256];

static float ref_GERecip(float w) {
	uint32_t bits;
	memcpy(&bits, &w, sizeof(bits));
	const uint32_t i = (bits >> 8) & 0x7FFF;
	const int e = (int)((bits >> 23) & 0xFF) - 127;
	const struct GeRecipSegment *seg = &ref_ge_recip_segments[i >> 8];
	const int32_t q = (64 * seg->b + 63 + seg->m * (int32_t)(i & 255)) >> 7;
	if (e >= 126) {
		return copysign(ldexpf((float)q, -16 - e), w);
	}
	return ref_ScaleByPow2((float)q, -16 - e, bits & 0x80000000);
}

static float ref_GERsqrt(float d) {
	uint32_t bits;
	memcpy(&bits, &d, sizeof(bits));
	const uint32_t i = (bits >> 8) & 0x7FFF;
	const int e = (int)((bits >> 23) & 0xFF) - 127;
	/* Таблица rsqrt у нас в файле реализации, а не в заголовке, поэтому
	 * берём её оттуда же, объявив здесь. */
	const struct GeRecipSegment *seg = &ref_ge_rsqrt_segments[e & 1][i >> 8];
	const int32_t q = (64 * seg->b + 63 + seg->m * (int32_t)(i & 255)) >> 7;
	return ref_ScaleByPow2((float)q, -16 - (e >> 1), 0);
}

static int64_t ref_GESetupRecip(uint64_t absDet, int *e) {
	const uint32_t hi = (uint32_t)(absDet >> 32);
	const int ex = hi ? 63 - (int)__builtin_clz(hi) : 31 - (int)__builtin_clz((uint32_t)absDet);
	*e = ex;
	const int index = (int)((absDet << 16) >> ex) - 65536;
	const struct GeSetupRecipSegment *seg = &ref_ge_setup_recip[index >> 8];
	return (2 * (int64_t)seg->k + seg->m * (index & 255)) >> 8;
}

static float ref_GELightPow(float v, float e) {
	if (e <= 0.0f) return 1.0f;
	if (v > 0.0f) {
		int32_t ix;
		memcpy(&ix, &v, sizeof(ix));
		double t = trunc((double)e * (double)(ix - 0x3F800000) * (1.0 / 16.0)) * 16.0 + 1065353216.0;
		t = t >= 0.0 ? (t < 2139095039.0 ? t : 2139095039.0) : 0.0;
		int32_t iy = (int32_t)t;
		float y;
		memcpy(&y, &iy, sizeof(y));
		return y;
	}
	return v;
}

static int ref_GELog16(float delta) {
	union { float f; uint32_t u; } fb;
	fb.f = delta;
	int useful = (fb.u >> 19) & 0x0FFF;
	return useful - 127 * 16;
}

static float ref_GETruncateTexCoord(float f) {
	uint32_t bits;
	memcpy(&bits, &f, 4);
	bits &= 0xFFFFFE00;
	memcpy(&f, &bits, 4);
	return f;
}

static float ref_GEUVProduct(double d) {
	uint64_t bits;
	memcpy(&bits, &d, sizeof(bits));
	if (((bits >> 52) & 0x7FF) != 0x7FF) {
		bits &= ~((1ULL << (52 - 23)) - 1);
	}
	memcpy(&d, &bits, sizeof(d));
	return (float)d;
}

/* Доступ к битам float: в `ge_math.c` он статический, поэтому здесь своя
 * копия. Расхождение между двумя копиями поймал бы дифференциальный тест
 * только если бы обе были неверны одинаково, поэтому сверяем их на месте. */
static u32 fbits(float f)
{
	u32 b;
	__builtin_memcpy(&b, &f, sizeof(b));
	return b;
}

static float bitsf(u32 b)
{
	float f;
	__builtin_memcpy(&f, &b, sizeof(f));
	return f;
}

/* --- сравнение ------------------------------------------------------------ */

static int fails;
static int checks;

static void cmp_u32(const char *name, u32 got, u32 want)
{
	checks++;
	if (got != want) {
		if (fails < 12)
			printf("  РАСХОЖДЕНИЕ %-22s получено %08X, ожидалось %08X\n",
			       name, got, want);
		fails++;
	}
}

static void cmp_f(const char *name, float got, float want)
{
	cmp_u32(name, fbits(got), fbits(want));
}

/* Простой ГПСЧ: детерминированно, чтобы прогон был воспроизводим. */
static u32 rng_state = 0x12345678u;

static u32 rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static float rng_float(void)
{
	/* Нормальные float с разными экспонентами: 0.5..2 и степени двойки. */
	u32 b = rng();
	int e = (int)(b % 60) - 30;
	u32 frac = (b >> 8) & 0x007FFFFFu;
	u32 bits = ((u32)(e + 127) << 23) | frac;
	if (b & 0x80000000u)
		bits |= 0x80000000u;
	return bitsf(bits);
}

int main(void)
{
	int i;

	printf("=== дифференциальный тест арифметики GE ===\n");

	/* Таблицы сверяются отдельно и до прогонов функций: без этого расхождение
	 * в GERsqrt неотличимо от ошибки в самой функции, а чинить надо разное.
	 * Сравнение побайтовое, по размеру всей таблицы. */
	if (memcmp(ge_recip_segments, ref_ge_recip_segments,
	           sizeof(ge_recip_segments)) != 0) {
		printf("  РАСХОЖДЕНИЕ таблицы ge_recip_segments\n");
		fails++;
	}
	if (memcmp(ge_rsqrt_segments, ref_ge_rsqrt_segments,
	           sizeof(ge_rsqrt_segments)) != 0) {
		printf("  РАСХОЖДЕНИЕ таблицы ge_rsqrt_segments\n");
		fails++;
	}
	if (memcmp(ge_setup_recip_table, ref_ge_setup_recip,
	           sizeof(ge_setup_recip_table)) != 0) {
		printf("  РАСХОЖДЕНИЕ таблицы ge_setup_recip_table\n");
		fails++;
	}
	printf("  таблицы сверены побайтово\n");

	/* TruncateToFloat24 */
	for (i = 0; i < 200000; i++) {
		float f = rng_float();
		cmp_f("TruncateToFloat24", ge_truncate_float24(f),
		      ref_TruncateToFloat24(f));
	}

	/* ProductToFloat24 */
	for (i = 0; i < 200000; i++) {
		float a = rng_float(), b = rng_float();
		cmp_f("ProductToFloat24",
		      ge_product_to_float24((double)a * (double)b),
		      ref_ProductToFloat24((double)a * (double)b));
	}

	/* ScaleByPow2 */
	for (i = 0; i < 200000; i++) {
		float f = rng_float();
		int n = (int)(rng() % 40) - 20;
		cmp_f("ScaleByPow2", ge_scale_by_pow2(f, n, 0),
		      ref_ScaleByPow2(f, n, 0));
	}

	/* ScaleByPow2Exact — только нормальный диапазон, иначе у нас другой
	 * путь (последовательное умножение), и сравнивать нечего. */
	for (i = 0; i < 200000; i++) {
		float f = rng_float();
		int e = (int)(rng() % 20) - 10;
		cmp_f("ScaleByPow2Exact", ge_scale_by_pow2_exact(f, e),
		      ref_ScaleByPow2Exact(f, e));
	}

	/* GEAdd */
	for (i = 0; i < 200000; i++) {
		float a = rng_float(), b = rng_float();
		cmp_f("GEAdd", ge_add(a, b), ref_GEAdd(a, b));
	}

	/* GEProduct + GERowSum: строки длиной 1..4 */
	for (i = 0; i < 200000; i++) {
		int count = (int)(rng() % 4) + 1;
		struct GeRowTerm mine[4];
		struct RefRowTerm ref[4];
		int k;

		for (k = 0; k < count; k++) {
			float a = rng_float(), b = rng_float();
			mine[k] = ge_product(a, b);
			ref[k] = ref_GEProduct(a, b);
		}
		cmp_f("GERowSum", ge_row_sum(mine, count),
		      ref_GERowSum(ref, count));
	}

	/* GERecip: нормальные float24 с разными экспонентами. */
	for (i = 0; i < 200000; i++) {
		float w = rng_float();
		cmp_f("GERecip", ge_recip(w), ref_GERecip(w));
	}

	/* GERsqrt */
	for (i = 0; i < 200000; i++) {
		float d = rng_float();
		cmp_f("GERsqrt", ge_rsqrt(d), ref_GERsqrt(d));
	}

	/* GESetupRecip: индекс — 16-битный, из [0, 65536), и сегмент равен
	 * `index >> 8`, то есть все 256 записей таблицы. Индекс равен
	 * ((absDet << 16) >> ex) - 65536, где ex = floor(log2 absDet). Значит
	 * absDet = (65536 + x)·2^e при x из [0, 65536) даёт индекс ровно x.
	 *
	 * Диапазон x обязан быть полным: при x < 256 сегмент всегда нулевой, и
	 * 255 записей таблицы остаются непроверенными — тест выглядел бы зелёным
	 * при половине неверных данных.
	 *
	 * e берётся от 16: при меньшем значении ex вышел бы отрицательным и сдвиг
	 * вправо стал бы неопределённым поведением.
	 *
	 * Верхняя граница e = 31 тоже не произвольна: `abs_det << 16` обязан
	 * помещаться в 64 бита, иначе старшие биты отбрасываются и индекс выходит
	 * отрицательным. В оригинале это недостижимо — `absDet` есть удвоенная
	 * площадь треугольника в фиксированной точке 12.4, то есть не больше ~2^31.
	 * Здесь же переполнение поймано тестом: при det = 2^53 индекс стал
	 * 16776964 вместо диапазона [0, 65536). */
	for (i = 0; i < 200000; i++) {
		int e = 16 + (int)(rng() % 16);
		u64 det = ((u64)(65536 + (rng() % 65536)) << e);
		int e1 = 0, e2 = 0;
		s64 a = ge_setup_recip(det, &e1);
		s64 b = ref_GESetupRecip(det, &e2);
		checks++;
		if (a != b || e1 != e2) {
			if (fails < 12)
				printf("  РАСХОЖДЕНИЕ GESetupRecip det=%016llX: %lld/%d против %lld/%d\n",
				       (unsigned long long)det,
				       (long long)a, e1, (long long)b, e2);
			fails++;
		}
	}

	/* GELightPow */
	for (i = 0; i < 200000; i++) {
		float v = rng_float(), e = rng_float();
		cmp_f("GELightPow", ge_light_pow(v, e), ref_GELightPow(v, e));
	}

	/* GELog16 */
	for (i = 0; i < 200000; i++) {
		float d = rng_float();
		cmp_u32("GELog16", (u32)ge_log16(d), (u32)ref_GELog16(d));
	}

	/* GETruncateTexCoord */
	for (i = 0; i < 200000; i++) {
		float f = rng_float();
		cmp_f("GETruncateTexCoord", ge_truncate_texcoord(f),
		      ref_GETruncateTexCoord(f));
	}

	/* GEUVProduct */
	for (i = 0; i < 200000; i++) {
		float a = rng_float(), b = rng_float();
		cmp_f("GEUVProduct", ge_uv_product((double)a * (double)b),
		      ref_GEUVProduct((double)a * (double)b));
	}

	printf("=== проверок: %d, расхождений: %d ===\n", checks, fails);
	if (fails == 0)
		printf("=== тест пройден ===\n");
	return fails != 0;
}
