
//          Copyright Gavin Band 2008 - 2012.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include <string>
#include <cassert>
#include <iostream>
#include <sstream>
#include <iomanip>
#include "appcontext/Timer.hpp"

namespace appcontext {
	double Timer::elapsed() const {
          std::chrono::duration<double> diff = std::chrono::system_clock::now()-now_pt;
          double ct = diff.count();
          return ct / 1000000000.0 ;
	}
	
	void Timer::restart() {
          now_pt=std::chrono::system_clock::now();
	}
	
	std::string Timer::display() const {
		std::ostringstream os ;
		os << std::fixed << std::setprecision(1) << elapsed() << "s" ;
		return os.str() ;
	}
}
