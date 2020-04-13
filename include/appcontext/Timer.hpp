
//          Copyright Gavin Band 2008 - 2012.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#ifndef TIMER_HPP
#define TIMER_HPP

#include <chrono>
#include <string>
#include <cassert>
#include <iostream>
#include <sstream>
#include <iomanip>

namespace appcontext {
	struct Timer
	{
		double elapsed() const ;
		void restart() ;
		std::string display() const ;
          Timer():now_pt(std::chrono::system_clock::now()){}
	private:
          std::chrono::time_point<std::chrono::system_clock> now_pt ;
	} ;
}

#endif
