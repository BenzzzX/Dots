#pragma once
#include "context.hpp"

namespace core
{
	namespace codebase
	{
		using handle_t = uint32_t;
		constexpr database::entity_type nulltype = {};

		enum class data_type
		{
			Column,
			LocalData,
			SharedData,
		};

		thread_local static data_flow* runing_dataflow;


		struct data_handle
		{
			data_flow* owner;
			data_type type;
			handle_t index;

			data_handle() = default;
			data_handle(const data_handle& other);
		};

		class local_data_pool
		{
			struct local_data
			{
				size_t offset;
				using destructor_t = void(*)(void*);
				std::function<void(void*)> constructor;
				destructor_t destructor;
			};
			std::vector<local_data> infos;
			size_t capacity;
			char* buffer;

			void realize(handle_t index)
			{
				auto& info = infos[index];
				info.constructor(buffer + info.offset);
			}
			void finalize(handle_t index)
			{
				auto& info = infos[index];
				info.destructor(buffer + info.offset);
			}
			void* get(handle_t index)
			{
				return buffer + infos[index].offset;
			}
		};

		struct kernel
		{
			data_flow* owner;
			handle_t index;

			void name(const char* n);
			void parallel(bool enable);
		};

		class data_flow
		{
			local_data_pool pool;
			context* cxt;

			struct data_info
			{
				struct component
				{
					std::string name;
					database::index_t type;
				};

				struct local_data
				{
					std::string name;
					size_t size;
					using destructor_t = void(*)(void*);
					std::function<void(void*)> constructor;
					destructor_t destructor;
				};

				struct shared_data
				{
					std::string name;
					void* ref;
				};

				std::vector<component> components;
				std::vector<local_data> localDatas;
				std::vector<shared_data> sharedDatas;
			} datas;

			struct rw_handle
			{
				data_type type;
				handle_t index;
				uint32_t stage;
			};

			struct data_node
			{
				data_type type;
				handle_t index;
				uint32_t stage;
				kernel from, to;
				std::vector<kernel> refs;
			};

			struct kernel_node
			{
				std::string name;
				std::function<void(context*)> action;
				std::vector<rw_handle> reads;
				std::vector<rw_handle> writes;
				std::optional<entity_filter> filter;
				constexpr static handle_t autoStage = -1;
				handle_t overrideGroup = autoStage;
				handle_t index;
				bool enableParallel;
				bool syncPoint;	
			};

			std::unordered_map<rw_handle, data_node> dataNodes;
			std::vector<kernel_node> kernelNodes;
			std::vector<data_handle> capturedData;
			friend data_handle;
			void on_data_captured(data_handle handle)
			{
				capturedData.push_back(handle);
			}

			struct query_cc
			{
				data_flow* owner;
				entity_filter filter;
				std::string n;

				void name(const char*);

				template<class F>
				kernel count(F&& f);

				template<class F>
				kernel map(F&& f);
			};

		public:

			template<class F>
			kernel map(F&& f);

			template<class F>
			kernel count(entity_filter& filter, F&& f);

			template<class F>
			kernel map(entity_filter& filter, F&& f);

			query_cc select(entity_filter& filter);
			
			entity_filter make_query(entity_type all = nulltype, 
				entity_type any = nulltype,
				entity_type none = nulltype, 
				entity_type changed = nulltype, 
				uint32_t timestamp = 0);

			template<class F>
			kernel maintain(F&& f);
		};

		template<class T>
		struct data : data_handle
		{
			T& operator*()
			{
				return *(T*)owner->pool.get(index);
			}

			T* operator->()
			{
				return (T*)owner->pool.get(index);
			}
		};

	}
}