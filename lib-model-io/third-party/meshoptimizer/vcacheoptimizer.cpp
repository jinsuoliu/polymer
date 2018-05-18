// This file is part of meshoptimizer library; see meshoptimizer.h for version/license details
#include "meshoptimizer.h"

#include <assert.h>
#include <math.h>
#include <string.h>

// This work is based on:
// Tom Forsyth. Linear-Speed Vertex Cache Optimisation. 2006
// Pedro Sander, Diego Nehab and Joshua Barczak. Fast Triangle Reordering for Vertex Locality and Reduced Overdraw. 2007
namespace meshopt
{

const size_t max_cache_size = 16;
const size_t max_valence = 8;

static const float vertex_score_table_cache[1 + max_cache_size] = {
    0.f,
    0.792f, 0.767f, 0.764f, 0.956f, 0.827f, 0.751f, 0.820f, 0.864f, 0.738f, 0.788f, 0.642f, 0.646f, 0.165f, 0.654f, 0.545f, 0.284f};

static const float vertex_score_table_live[1 + max_valence] = {
    0.f,
    0.994f, 0.721f, 0.479f, 0.423f, 0.174f, 0.080f, 0.249f, 0.056f};

struct Adjacency
{
	meshopt_Buffer<unsigned int> triangle_counts;
	meshopt_Buffer<unsigned int> offsets;
	meshopt_Buffer<unsigned int> data;

	Adjacency(size_t index_count, size_t vertex_count)
	    : triangle_counts(vertex_count)
	    , offsets(vertex_count)
	    , data(index_count)
	{
	}
};

static void buildAdjacency(Adjacency& adjacency, const unsigned int* indices, size_t index_count, size_t vertex_count)
{
	size_t face_count = index_count / 3;

	// fill triangle counts
	for (size_t i = 0; i < vertex_count; ++i)
	{
		adjacency.triangle_counts[i] = 0;
	}

	for (size_t i = 0; i < index_count; ++i)
	{
		assert(indices[i] < vertex_count);

		adjacency.triangle_counts[indices[i]]++;
	}

	// fill offset table
	unsigned int offset = 0;

	for (size_t i = 0; i < vertex_count; ++i)
	{
		adjacency.offsets[i] = offset;
		offset += adjacency.triangle_counts[i];
	}

	assert(offset == index_count);

	// fill triangle data
	for (size_t i = 0; i < face_count; ++i)
	{
		unsigned int a = indices[i * 3 + 0], b = indices[i * 3 + 1], c = indices[i * 3 + 2];

		adjacency.data[adjacency.offsets[a]++] = unsigned(i);
		adjacency.data[adjacency.offsets[b]++] = unsigned(i);
		adjacency.data[adjacency.offsets[c]++] = unsigned(i);
	}

	// fix offsets that have been disturbed by the previous pass
	for (size_t i = 0; i < vertex_count; ++i)
	{
		assert(adjacency.offsets[i] >= adjacency.triangle_counts[i]);

		adjacency.offsets[i] -= adjacency.triangle_counts[i];
	}
}

static unsigned int getNextVertexDeadEnd(const unsigned int* dead_end, unsigned int& dead_end_top, unsigned int& input_cursor, const unsigned int* live_triangles, size_t vertex_count)
{
	// check dead-end stack
	while (dead_end_top)
	{
		unsigned int vertex = dead_end[--dead_end_top];

		if (live_triangles[vertex] > 0)
			return vertex;
	}

	// input order
	while (input_cursor < vertex_count)
	{
		if (live_triangles[input_cursor] > 0)
			return input_cursor;

		++input_cursor;
	}

	return ~0u;
}

static unsigned int getNextVertexNeighbour(const unsigned int* next_candidates_begin, const unsigned int* next_candidates_end, const unsigned int* live_triangles, const unsigned int* cache_timestamps, unsigned int timestamp, unsigned int cache_size)
{
	unsigned int best_candidate = ~0u;
	int best_priority = -1;

	for (const unsigned int* next_candidate = next_candidates_begin; next_candidate != next_candidates_end; ++next_candidate)
	{
		unsigned int vertex = *next_candidate;

		// otherwise we don't need to process it
		if (live_triangles[vertex] > 0)
		{
			int priority = 0;

			// will it be in cache after fanning?
			if (2 * live_triangles[vertex] + timestamp - cache_timestamps[vertex] <= cache_size)
			{
				priority = timestamp - cache_timestamps[vertex]; // position in cache
			}

			if (priority > best_priority)
			{
				best_candidate = vertex;
				best_priority = priority;
			}
		}
	}

	return best_candidate;
}

static float vertexScore(int cache_position, unsigned int live_triangles)
{
	assert(cache_position >= -1 && cache_position < int(max_cache_size));

	unsigned int live_triangles_clamped = live_triangles < max_valence ? live_triangles : max_valence;

	return vertex_score_table_cache[1 + cache_position] + vertex_score_table_live[live_triangles_clamped];
}

static unsigned int getNextTriangleDeadEnd(unsigned int& input_cursor, const char* emitted_flags, size_t face_count)
{
	// input order
	while (input_cursor < face_count)
	{
		if (!emitted_flags[input_cursor])
			return input_cursor;

		++input_cursor;
	}

	return ~0u;
}

} // namespace meshopt

void meshopt_optimizeVertexCache(unsigned int* destination, const unsigned int* indices, size_t index_count, size_t vertex_count)
{
	using namespace meshopt;

	assert(index_count % 3 == 0);

	// guard for empty meshes
	if (index_count == 0 || vertex_count == 0)
		return;

	// support in-place optimization
	meshopt_Buffer<unsigned int> indices_copy;

	if (destination == indices)
	{
		indices_copy.allocate(index_count);
		memcpy(indices_copy.data, indices, index_count * sizeof(unsigned int));
		indices = indices_copy.data;
	}

	unsigned int cache_size = 16;
	assert(cache_size <= max_cache_size);

	size_t face_count = index_count / 3;

	// build adjacency information
	Adjacency adjacency(index_count, vertex_count);
	buildAdjacency(adjacency, indices, index_count, vertex_count);

	// live triangle counts
	meshopt_Buffer<unsigned int> live_triangles(vertex_count);
	memcpy(live_triangles.data, adjacency.triangle_counts.data, vertex_count * sizeof(unsigned int));

	// emitted flags
	meshopt_Buffer<char> emitted_flags(face_count);
	memset(emitted_flags.data, 0, face_count);

	// compute initial vertex scores
	meshopt_Buffer<float> vertex_scores(vertex_count);

	for (size_t i = 0; i < vertex_count; ++i)
	{
		vertex_scores[i] = vertexScore(-1, live_triangles[i]);
	}

	// compute triangle scores
	meshopt_Buffer<float> triangle_scores(face_count);

	for (size_t i = 0; i < face_count; ++i)
	{
		unsigned int a = indices[i * 3 + 0];
		unsigned int b = indices[i * 3 + 1];
		unsigned int c = indices[i * 3 + 2];

		triangle_scores[i] = vertex_scores[a] + vertex_scores[b] + vertex_scores[c];
	}

	unsigned int cache_holder[2 * (max_cache_size + 3)];
	unsigned int* cache = cache_holder;
	unsigned int* cache_new = cache_holder + max_cache_size + 3;
	size_t cache_count = 0;

	unsigned int current_triangle = 0;
	unsigned int input_cursor = 1;

	unsigned int output_triangle = 0;

	while (current_triangle != ~0u)
	{
		assert(output_triangle < face_count);

		unsigned int a = indices[current_triangle * 3 + 0];
		unsigned int b = indices[current_triangle * 3 + 1];
		unsigned int c = indices[current_triangle * 3 + 2];

		// output indices
		destination[output_triangle * 3 + 0] = a;
		destination[output_triangle * 3 + 1] = b;
		destination[output_triangle * 3 + 2] = c;
		output_triangle++;

		// update emitted flags
		emitted_flags[current_triangle] = true;
		triangle_scores[current_triangle] = 0;

		// new triangle
		size_t cache_write = 0;
		cache_new[cache_write++] = a;
		cache_new[cache_write++] = b;
		cache_new[cache_write++] = c;

		// old triangles
		for (size_t i = 0; i < cache_count; ++i)
		{
			unsigned int index = cache[i];

			if (index != a && index != b && index != c)
			{
				cache_new[cache_write++] = index;
			}
		}

		unsigned int* cache_temp = cache;
		cache = cache_new, cache_new = cache_temp;
		cache_count = cache_write > cache_size ? cache_size : cache_write;

		// update live triangle counts
		live_triangles[a]--;
		live_triangles[b]--;
		live_triangles[c]--;

		// remove emitted triangle from adjacency data
		// this makes sure that we spend less time traversing these lists on subsequent iterations
		for (size_t k = 0; k < 3; ++k)
		{
			unsigned int index = indices[current_triangle * 3 + k];

			unsigned int* neighbours = &adjacency.data[0] + adjacency.offsets[index];
			size_t neighbours_size = adjacency.triangle_counts[index];

			for (size_t i = 0; i < neighbours_size; ++i)
			{
				unsigned int tri = neighbours[i];

				if (tri == current_triangle)
				{
					neighbours[i] = neighbours[neighbours_size - 1];
					adjacency.triangle_counts[index]--;
					break;
				}
			}
		}

		unsigned int best_triangle = ~0u;
		float best_score = 0;

		// update cache positions, vertex scores and triangle scores, and find next best triangle
		for (size_t i = 0; i < cache_write; ++i)
		{
			unsigned int index = cache[i];

			int cache_position = i >= cache_size ? -1 : int(i);

			// update vertex score
			float score = vertexScore(cache_position, live_triangles[index]);
			float score_diff = score - vertex_scores[index];

			vertex_scores[index] = score;

			// update scores of vertex triangles
			const unsigned int* neighbours_begin = &adjacency.data[0] + adjacency.offsets[index];
			const unsigned int* neighbours_end = neighbours_begin + adjacency.triangle_counts[index];

			for (const unsigned int* it = neighbours_begin; it != neighbours_end; ++it)
			{
				unsigned int tri = *it;
				assert(!emitted_flags[tri]);

				float tri_score = triangle_scores[tri] + score_diff;
				assert(tri_score > 0);

				if (best_score < tri_score)
				{
					best_triangle = tri;
					best_score = tri_score;
				}

				triangle_scores[tri] = tri_score;
			}
		}

		// step through input triangles in order if we hit a dead-end
		current_triangle = best_triangle;

		if (current_triangle == ~0u)
		{
			current_triangle = getNextTriangleDeadEnd(input_cursor, &emitted_flags[0], face_count);
		}
	}

	assert(input_cursor == face_count);
	assert(output_triangle == face_count);
}

void meshopt_optimizeVertexCacheFifo(unsigned int* destination, const unsigned int* indices, size_t index_count, size_t vertex_count, unsigned int cache_size)
{
	using namespace meshopt;

	assert(index_count % 3 == 0);
	assert(cache_size >= 3);

	// guard for empty meshes
	if (index_count == 0 || vertex_count == 0)
		return;

	// support in-place optimization
	meshopt_Buffer<unsigned int> indices_copy;

	if (destination == indices)
	{
		indices_copy.allocate(index_count);
		memcpy(indices_copy.data, indices, index_count * sizeof(unsigned int));
		indices = indices_copy.data;
	}

	size_t face_count = index_count / 3;

	// build adjacency information
	Adjacency adjacency(index_count, vertex_count);
	buildAdjacency(adjacency, indices, index_count, vertex_count);

	// live triangle counts
	meshopt_Buffer<unsigned int> live_triangles(vertex_count);
	memcpy(live_triangles.data, adjacency.triangle_counts.data, vertex_count * sizeof(unsigned int));

	// cache time stamps
	meshopt_Buffer<unsigned int> cache_timestamps(vertex_count);
	memset(cache_timestamps.data, 0, vertex_count * sizeof(unsigned int));

	// dead-end stack
	meshopt_Buffer<unsigned int> dead_end(index_count);
	unsigned int dead_end_top = 0;

	// emitted flags
	meshopt_Buffer<char> emitted_flags(face_count);
	memset(emitted_flags.data, 0, face_count);

	unsigned int current_vertex = 0;

	unsigned int timestamp = cache_size + 1;
	unsigned int input_cursor = 1; // vertex to restart from in case of dead-end

	unsigned int output_triangle = 0;

	while (current_vertex != ~0u)
	{
		const unsigned int* next_candidates_begin = &dead_end[0] + dead_end_top;

		// emit all vertex neighbours
		const unsigned int* neighbours_begin = &adjacency.data[0] + adjacency.offsets[current_vertex];
		const unsigned int* neighbours_end = neighbours_begin + adjacency.triangle_counts[current_vertex];

		for (const unsigned int* it = neighbours_begin; it != neighbours_end; ++it)
		{
			unsigned int triangle = *it;

			if (!emitted_flags[triangle])
			{
				unsigned int a = indices[triangle * 3 + 0], b = indices[triangle * 3 + 1], c = indices[triangle * 3 + 2];

				// output indices
				destination[output_triangle * 3 + 0] = a;
				destination[output_triangle * 3 + 1] = b;
				destination[output_triangle * 3 + 2] = c;
				output_triangle++;

				// update dead-end stack
				dead_end[dead_end_top + 0] = a;
				dead_end[dead_end_top + 1] = b;
				dead_end[dead_end_top + 2] = c;
				dead_end_top += 3;

				// update live triangle counts
				live_triangles[a]--;
				live_triangles[b]--;
				live_triangles[c]--;

				// update cache info
				// if vertex is not in cache, put it in cache
				if (timestamp - cache_timestamps[a] > cache_size)
					cache_timestamps[a] = timestamp++;

				if (timestamp - cache_timestamps[b] > cache_size)
					cache_timestamps[b] = timestamp++;

				if (timestamp - cache_timestamps[c] > cache_size)
					cache_timestamps[c] = timestamp++;

				// update emitted flags
				emitted_flags[triangle] = true;
			}
		}

		// next candidates are the ones we pushed to dead-end stack just now
		const unsigned int* next_candidates_end = &dead_end[0] + dead_end_top;

		// get next vertex
		current_vertex = getNextVertexNeighbour(next_candidates_begin, next_candidates_end, &live_triangles[0], &cache_timestamps[0], timestamp, cache_size);

		if (current_vertex == ~0u)
		{
			current_vertex = getNextVertexDeadEnd(&dead_end[0], dead_end_top, input_cursor, &live_triangles[0], vertex_count);
		}
	}

	assert(output_triangle == face_count);
}
