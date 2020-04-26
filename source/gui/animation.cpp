/*
 *	An Animation Implementation
 *	Nana C++ Library(http://www.nanapro.org)
 *	Copyright(C) 2003-2020 Jinhao(cnjinhao@hotmail.com)
 *
 *	Distributed under the Boost Software License, Version 1.0.
 *	(See accompanying file LICENSE_1_0.txt or copy at
 *	http://www.boost.org/LICENSE_1_0.txt)
 *
 *	@file: nana/gui/animation.cpp
 */


#include <nana/gui/animation.hpp>
#include <nana/gui/drawing.hpp>
#include <nana/system/timepiece.hpp>
#include <nana/system/platform.hpp>

#include <vector>
#include <list>
#include <map>
#include <algorithm>
#include <atomic>

#include <thread>
#include <mutex>
#include <condition_variable>

namespace nana
{
	class animation;

	struct output_t
	{
		drawing::diehard_t diehard{ nullptr };
		std::vector<nana::point> points;
	};

	struct framebuilder
	{
		std::size_t length;
		std::function<bool(std::size_t, paint::graphics&, nana::size&)> frbuilder;

		framebuilder(std::function<bool(std::size_t, paint::graphics&, nana::size&)> f, std::size_t l)
			: length(l), frbuilder(std::move(f))
		{}
	};

	struct frame
	{
		enum class kind
		{
			image,
			framebuilder
		};

		frame(paint::image img, std::size_t duration):
			duration(duration),
			type(kind::image)
		{
			u.image = new paint::image(std::move(img));
		}

		frame(std::function<bool(std::size_t, paint::graphics&, nana::size&)> frbuilder, std::size_t length):
			type(kind::framebuilder)
		{
			u.frbuilder = new framebuilder(std::move(frbuilder), length);
		}

		frame(const frame& r)
			: type(r.type)
		{
			switch(type)
			{
			case kind::image:
				u.image = new paint::image(*r.u.image);
				break;
			case kind::framebuilder:
				u.frbuilder = new framebuilder(*r.u.frbuilder);
				break;
			}
		}

		frame(frame&& r)
			: type(r.type)
		{
			u = r.u;
			r.u.image = nullptr;
		}

		~frame()
		{
			switch(type)
			{
			case kind::image:
				delete u.image;
				break;
			case kind::framebuilder:
				delete u.frbuilder;
				break;
			}
		}

		frame& operator=(const frame& r)
		{
			if(this != &r)
			{
				switch(type)
				{
				case kind::image:
					delete u.image;
					break;
				case kind::framebuilder:
					delete u.frbuilder;
					break;
				}

				type = r.type;
				switch(type)
				{
				case kind::image:
					u.image = new paint::image(*r.u.image);
					break;
				case kind::framebuilder:
					u.frbuilder = new framebuilder(*r.u.frbuilder);
					break;
				}
			}
			return *this;
		}

		frame& operator=(frame&& r)
		{
			if(this != &r)
			{
				switch(type)
				{
				case kind::image:
					delete u.image;
					break;
				case kind::framebuilder:
					delete u.frbuilder;
					break;
				}

				type = r.type;
				u = r.u;
				r.u.image = nullptr;
			}
			return *this;
		}

		std::size_t length() const
		{
			switch(type)
			{
			case kind::image:
				return 1;
			case kind::framebuilder:
				return u.frbuilder->length;
			}
			return 0;
		}

		//
		std::size_t duration{0};
		kind type;
		union uframes
		{
			paint::image * image;
			framebuilder * frbuilder;
		}u;
	};

	//class frameset
		//struct frameset::impl
		struct frameset::impl
		{
			//Only list whose iterator would not be invalidated after an insertion.
			std::list<frame> frames;
			std::list<frame>::iterator this_frame;
			std::size_t pos_in_this_frame{ 0 };
			mutable bool good_frame_by_frmbuilder{ false };	//It indicates the state of frame whether is valid.

			impl()
				:	this_frame(frames.end())
			{}

			//Renders a frame on the set of windows.
			std::size_t render_this(std::map<window, output_t>& outs, paint::graphics& framegraph, nana::size& framegraph_dimension) const
			{
				if(this_frame == frames.end())
					return 0;

				frame & frmobj = *this_frame;
				switch(frmobj.type)
				{
				case frame::kind::image:
					_m_render(outs, [&frmobj](paint::graphics& tar, const nana::point& pos)
					{
						frmobj.u.image->paste(tar, pos);
					});
					if(0 == frmobj.duration)
						return frmobj.u.image->frame_duration();
					break;
				case frame::kind::framebuilder:
					good_frame_by_frmbuilder = frmobj.u.frbuilder->frbuilder(pos_in_this_frame, framegraph, framegraph_dimension);
					if(good_frame_by_frmbuilder)
					{
						nana::rectangle r(framegraph_dimension);
						_m_render(outs, [&r, &framegraph](paint::graphics& tar, const nana::point& pos) mutable
						{
							r.x = pos.x;
							r.y = pos.y;
							tar.bitblt(r, framegraph);
						});
					}
					break;
				}

				return frmobj.duration;
			}

			//Render a frame on a specified window graph
			std::size_t render_this(paint::graphics& graph, const nana::point& pos, paint::graphics& framegraph, nana::size& framegraph_dimension, bool rebuild_frame) const
			{
				if(this_frame == frames.end())
					return 0;

				frame & frmobj = *this_frame;
				switch(frmobj.type)
				{
				case frame::kind::image:
					frmobj.u.image->paste(graph, pos);
					if(0 == frmobj.duration)
						return frmobj.u.image->frame_duration();
					break;
				case frame::kind::framebuilder:
					if(rebuild_frame)
						good_frame_by_frmbuilder = frmobj.u.frbuilder->frbuilder(pos_in_this_frame, framegraph, framegraph_dimension);

					if(good_frame_by_frmbuilder)
					{
						nana::rectangle r(pos, framegraph_dimension);
						graph.bitblt(r, framegraph);
					}
					break;
				}
				return frmobj.duration;
			}

			bool eof() const
			{
				return (frames.end() == this_frame);
			}

			void next_frame()
			{
				if(frames.end() == this_frame)
					return;

				frame & frmobj = *this_frame;
				switch(frmobj.type)
				{
				case frame::kind::image:
					if(!frmobj.u.image->set_frame(++pos_in_this_frame))
					{
						++this_frame;
						pos_in_this_frame = 0;
						frmobj.u.image->set_frame(0);
					}
					break;
				case frame::kind::framebuilder:
					if(pos_in_this_frame >= frmobj.u.frbuilder->length)
					{
						pos_in_this_frame = 0;
						++this_frame;
					}
					else
						++pos_in_this_frame;
					break;
				default:
					throw std::runtime_error("Nana.GUI.Animation: Bad frame type");
				}
			}

			//Seek to the first frame
			void reset()
			{
				this_frame = frames.begin();
				pos_in_this_frame = 0;
			}
		private:
			template<typename Renderer>
			void _m_render(std::map<window, output_t>& outs, Renderer renderer) const
			{
				for(auto & tar: outs)
				{
					auto graph = api::dev::window_graphics(tar.first);
					if(nullptr == graph)
						continue;

					for(auto & outp : tar.second.points)
						renderer(*graph, outp);

					api::update_window(tar.first);
				}
			}
		};//end struct frameset::impl
	//public:
		frameset::frameset()
			: impl_(std::make_unique<impl>())
		{}

		void frameset::push_back(paint::image img, std::size_t duration)
		{
			bool located = impl_->this_frame != impl_->frames.end();
			impl_->frames.emplace_back(std::move(img), duration);
			if(false == located)
				impl_->this_frame = impl_->frames.begin();
		}

		void frameset::push_back(framebuilder fb, std::size_t length)
		{
			impl_->frames.emplace_back(std::move(fb), length);
			if(1 == impl_->frames.size())
				impl_->this_frame = impl_->frames.begin();
		}
	//end class frameset

	//class animation
		class animation::performance_manager
		{
		public:
			struct control_block
			{
				impl* ani;
				std::size_t duration_left{ 0 };
			};

			struct thread_variable
			{
				std::mutex mutex;
				std::condition_variable condvar;
				std::vector<control_block> animations;

				std::size_t active;				//The number of active animations
				std::shared_ptr<std::thread> thread;

				std::size_t fps;
				double interval;	//milliseconds between 2 frames.
				double performance_parameter;
			};

			void insert(impl* p);
			void set_fps(impl*, std::size_t new_fps);
			void close(impl* p);
			bool empty() const;
		private:
			mutable std::recursive_mutex mutex_;
			std::vector<thread_variable*> threads_;
		};	//end class animation::performance_manager

		struct animation::impl
		{
			bool	looped{false};
			std::atomic<bool>	paused{true};
			std::size_t fps;

			std::list<frameset> framesets;
			std::map<std::string, branch_t> branches;
			std::map<window, output_t> outputs;

			paint::graphics framegraph;	//framegraph will be created by framebuilder
			nana::size framegraph_dimension;

			struct state_t
			{
				std::list<frameset>::iterator this_frameset;
			}state;

			performance_manager::thread_variable * thr_variable;
			static performance_manager * perf_manager;


			impl(std::size_t fps)
				: fps(fps)
			{
				state.this_frameset = framesets.begin();

				if (!perf_manager)
				{
					nana::internal_scope_guard lock;
					if (!perf_manager)
					{
						auto pm = new performance_manager;
						perf_manager = pm;
					}
				}
				perf_manager->insert(this);
			}

			~impl()
			{
				perf_manager->close(this);
				{
					nana::internal_scope_guard lock;
					if(perf_manager->empty())
					{
						delete perf_manager;
						perf_manager = nullptr;
					}
				}
			}

			std::size_t render_this_specifically(paint::graphics& graph, const nana::point& pos)
			{
				if(state.this_frameset != framesets.end())
					return state.this_frameset->impl_->render_this(graph, pos, framegraph, framegraph_dimension, false);

				return 0;
			}

			std::size_t render_this_frame()
			{
				if(state.this_frameset != framesets.end())
					return state.this_frameset->impl_->render_this(outputs, framegraph, framegraph_dimension);

				return 0;
			}

			bool move_to_next()
			{
				if(state.this_frameset != framesets.end())
				{
					state.this_frameset->impl_->next_frame();
					return (!state.this_frameset->impl_->eof());
				}
				return false;
			}

			//Seek to the first frameset
			void reset()
			{
				state.this_frameset = framesets.begin();
				if(state.this_frameset != framesets.end())
					state.this_frameset->impl_->reset();
			}
		};//end struct animation::impl

		//class animation::performance_manager
			void animation::performance_manager::insert(impl* p)
			{
				std::lock_guard<decltype(mutex_)> lock(mutex_);
				for(auto thr : threads_)
				{
					std::lock_guard<decltype(thr->mutex)> privlock(thr->mutex);
					if (thr->fps == p->fps)
					{
						if (thr->animations.empty() || (thr->performance_parameter * (1.0 + 1.0 / thr->animations.size()) <= 43.3))
						{
							p->thr_variable = thr;
							thr->animations.emplace_back(control_block{p});
							return;
						}
					}
				}

				auto thr = std::make_unique<thread_variable>();
				thr->animations.emplace_back(control_block{p});
				thr->performance_parameter = 0.0;
				thr->fps = p->fps;
				thr->interval = 1000.0 / double(p->fps);
				thr->thread = std::make_shared<std::thread>([thr = thr.get()]()
				{
					nana::system::timepiece tmpiece;
					while (true)
					{
						std::size_t lowest_duration = std::numeric_limits<std::size_t>::max();

						tmpiece.start();

						{
							std::lock_guard<decltype(thr->mutex)> lock(thr->mutex);
							thr->active = 0;

							for (auto& cb : thr->animations)
							{
								if (cb.ani->paused)
									continue;

								++thr->active;

								if(0 == cb.duration_left)
								{
									auto dur = cb.ani->render_this_frame();
									cb.duration_left = dur ? dur : std::size_t(thr->interval);

									if (false == cb.ani->move_to_next())
									{
										if (cb.ani->looped)
											cb.ani->reset();
										else
											--thr->active;
									}
								}


								if(cb.duration_left < lowest_duration)
									lowest_duration = cb.duration_left;
							}

							for(auto & cb : thr->animations)
							{
								if(!cb.ani->paused)
									cb.duration_left -= lowest_duration;
							}
						}

						if (thr->active)
						{
							thr->performance_parameter = tmpiece.calc();
							if (thr->performance_parameter < lowest_duration)
								std::this_thread::sleep_for(std::chrono::milliseconds{static_cast<int>(lowest_duration - thr->performance_parameter)});
						}
						else
						{
							//There isn't an active frame, then let the thread
							//wait for a signal for an active animation
							std::unique_lock<std::mutex> lock(thr->mutex);
							if (0 == thr->active)
								thr->condvar.wait(lock);
						}
					}
				});

				threads_.push_back(thr.release());
				p->thr_variable = threads_.back();
			}

			void animation::performance_manager::set_fps(impl* p, std::size_t new_fps)
			{
				if (p->fps == new_fps)
					return;

				std::lock_guard<decltype(mutex_)> lock(mutex_);
				auto i = std::find(threads_.begin(), threads_.end(), p->thr_variable);
				if (i == threads_.end())
					return;

				p->fps = new_fps;
				auto thr = *i;

				//Simply modify the fps parameter if the thread just has one animation.
				if (thr->animations.size() == 1)
				{
					thr->fps = new_fps;
					thr->interval = 1000.0 / double(new_fps);
					return;
				}

				std::lock_guard<decltype(thr->mutex)> privlock(thr->mutex);

				for(auto i = thr->animations.cbegin(); i != thr->animations.cend(); ++i)
					if(i->ani == p)
					{
						thr->animations.erase(i);
						break;
					}
				
				p->thr_variable = nullptr;
				insert(p);
			}

			void animation::performance_manager::close(impl* p)
			{
				std::lock_guard<decltype(mutex_)> lock(mutex_);
				auto i = std::find(threads_.begin(), threads_.end(), p->thr_variable);
				if (i == threads_.end())
					return;

				auto thr = *i;
				std::lock_guard<decltype(thr->mutex)> privlock(thr->mutex);

				for(auto i = thr->animations.cbegin(); i != thr->animations.cend(); ++i)
					if(i->ani == p)
					{
						thr->animations.erase(i);
						break;
					}

				p->thr_variable = nullptr;
			}

			bool animation::performance_manager::empty() const
			{
				std::lock_guard<decltype(mutex_)> lock(mutex_);
				for(auto thr : threads_)
				{
					if(thr->animations.size())
						return false;
				}
				return true;
			}
		//end class animation::performance_manager

		animation::animation(std::size_t fps)
			: impl_(std::make_unique<impl>(fps))
		{
		}

		animation::~animation() = default;

		animation::animation(animation&& rhs)
			: impl_(std::move(rhs.impl_))
		{
			rhs.impl_ = std::make_unique<impl>(23);
		}

		animation& animation::operator=(animation&& rhs)
		{
			if (this != &rhs)
			{
				std::swap(rhs.impl_, this->impl_);
			}
			return *this;
		}

		void animation::push_back(frameset frms)
		{
			impl_->framesets.emplace_back(std::move(frms));
			if(1 == impl_->framesets.size())
				impl_->state.this_frameset = impl_->framesets.begin();
		}

		void animation::push_back(paint::image img, std::size_t duration)
		{
			if(img)
			{
				frameset fs;
				fs.push_back(std::move(img), duration);
				push_back(std::move(fs));
			}
		}

		/*
		void branch(const std::string& name, const frameset& frms)
		{
			impl_->branches[name].frames = frms;
		}

		void branch(const std::string& name, const frameset& frms, std::function<std::size_t(const std::string&, std::size_t, std::size_t&)> condition)
		{
			auto & br = impl_->branches[name];
			br.frames = frms;
			br.condition = condition;
		}
		*/

		void animation::looped(bool enable)
		{
			if(impl_->looped != enable)
			{
				impl_->looped = enable;
				if(enable)
				{
					std::unique_lock<std::mutex> lock(impl_->thr_variable->mutex);
					if(0 == impl_->thr_variable->active)
					{
						impl_->thr_variable->active = 1;
						impl_->thr_variable->condvar.notify_one();
					}
				}
			}
		}

		void animation::play()
		{
			impl_->paused = false;
			std::unique_lock<std::mutex> lock(impl_->thr_variable->mutex);
			if(0 == impl_->thr_variable->active)
			{
				impl_->thr_variable->active = 1;
				impl_->thr_variable->condvar.notify_one();
			}
		}

		void animation::pause()
		{
			impl_->paused = true;
		}

		void animation::output(window wd, const nana::point& pos)
		{
			auto & output = impl_->outputs[wd];

			if(nullptr == output.diehard)
			{
				drawing dw(wd);
				output.diehard = dw.draw_diehard([this, pos](paint::graphics& tar){
					impl_->render_this_specifically(tar, pos);
				});

				api::events(wd).destroy.connect([this](const arg_destroy& arg){
					std::lock_guard<decltype(impl_->thr_variable->mutex)> lock(impl_->thr_variable->mutex);
					impl_->outputs.erase(arg.window_handle);
				});
			}
			output.points.push_back(pos);
		}

		void animation::fps(std::size_t n)
		{
			if (n == impl_->fps)
				return;

			impl::perf_manager->set_fps(impl_.get(), n);
		}

		std::size_t animation::fps() const
		{
			return impl_->fps;
		}
	//end class animation


	animation::performance_manager * animation::impl::perf_manager;
}	//end namespace nana

