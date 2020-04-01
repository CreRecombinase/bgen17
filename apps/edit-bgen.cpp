
//          Copyright Gavin Band 2008 - 2012.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include <string>
#include <iostream>
#include <fstream>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/ptr_container/ptr_vector.hpp>
#include <algorithm>
#include "genfile/bgen/bgen.hpp"
#include "appcontext/CmdLineOptionProcessor.hpp"
#include "appcontext/OptionProcessor.hpp"
#include "appcontext/ApplicationContext.hpp"
#include "config.h"

namespace globals {
	std::string const program_name = "edit-bgen" ;
	std::string const program_version = bgen_revision ;
}

struct EditBgenOptionProcessor: public appcontext::CmdLineOptionProcessor
{
public:
	std::string get_program_name() const { return globals::program_name ; }

	void declare_options( appcontext::OptionProcessor& options ) {
		// Meta-options
		options.set_help_option( "-help" ) ;

		options.declare_group( "Input / output file options" ) ;
	    options[ "-g" ]
	        .set_description(
				"Path of bgen file(s) to edit. "
			)
			.set_takes_values_until_next_option()
			.set_is_required()
		;

		options.declare_group( "Actions" ) ;
	    options[ "-set-free-data" ]
	        .set_description(
				"Set new 'free data' field. The argument must be a string with length exactly equal to the length of the existing free data field in each edited file."
			)
			.set_takes_single_value()
		;

	    options[ "-remove-sample-identifiers" ]
	        .set_description(
				"Remove sample identifiers from the file.  This zeroes out the sample ID block, if present."
			)
		;
		
		options[ "-really" ]
			.set_description( "Really make changes (without this option a dry run is performed with no changes to files.)" ) ;
	}
} ;

struct EditBgenApplication: public appcontext::ApplicationContext
{
public:
	EditBgenApplication( int argc, char** argv ):
		appcontext::ApplicationContext(
			globals::program_name,
			globals::program_version,
			std::auto_ptr< appcontext::OptionProcessor >( new EditBgenOptionProcessor ),
			argc,
			argv,
			"-log"
		)
	{
		process() ;
	}

	void process() {
		try {
			unsafe_process() ;
		}
		catch( std::invalid_argument const& e ) {
			ui().logger() << "\nError: " << e.what() <<".\n" ;
			throw appcontext::HaltProgramWithReturnCode( -1 ) ;
		}
	}
	
	void unsafe_process() {
		std::vector< std::string > filenames = options().get_values< std::string >( "-g" ) ;
		std::auto_ptr< boost::ptr_vector< std::fstream > > streams  = open_bgen_files( filenames ) ;

		bool somethingDone = false ;
		if( options().check( "-set-free-data" )) {
			somethingDone = true ;
			std::string const free_data = options().get< std::string >( "-set-free-data" ) ;
			edit_free_data( filenames, *streams, free_data, options().check( "-really" ) ) ;
		}
		
		if( options().check( "-remove-sample-identifiers" )) {
			somethingDone = true ;
			remove_sample_identifiers( filenames, *streams, options().check( "-really" ) ) ;
		}
		
		if( !somethingDone ) {
			ui().logger() << "!! Nothing to do.\n" ;
		}
	}
	
	std::auto_ptr< boost::ptr_vector< std::fstream > > open_bgen_files( std::vector< std::string > const& filenames ) const {
		boost::ptr_vector< std::fstream > streams ;
		for( std::size_t i = 0; i < filenames.size(); ++i ) {
			streams.push_back(
				std::auto_ptr< std::fstream >(
					new std::fstream(
						filenames[i].c_str(),
						std::ios::in | std::ios::out | std::ios::binary
					)
				)
			) ;
		}
		return streams.release() ;
	}
	
	void edit_free_data(
		std::vector< std::string > const& filenames,
		boost::ptr_vector< std::fstream >& streams,
		std::string const& free_data,
		bool really
	) const {
		assert( filenames.size() == streams.size() ) ;
		for( std::size_t i = 0; i < filenames.size(); ++i ) {
			edit_free_data( filenames[i], streams[i], free_data, really ) ;
		}
	}

	void edit_free_data(
		std::string const& filename,
		std::fstream& stream,
		std::string const& free_data,
		bool really
	) const {
		ui().logger() << boost::format( "Setting free data for \"%s\" to \"%s\"..." ) % filename % free_data ;

		// Read (and double-check) the header
		// We checked this earlier, so assert if this fails.
		stream.seekg( 4 ) ;
		genfile::bgen::Context context ;
		genfile::bgen::read_header_block( stream, &context ) ;
		if( context.free_data.size() != free_data.size() ) {
			ui().logger() << (
				boost::format( "In bgen file \"%s\": size of new free data (%d bytes) does not match that of free data in file (\"%s\", %d bytes)." )
					% filename % free_data.size() % context.free_data % context.free_data.size()
			).str() ;
			throw std::invalid_argument( "filename=\"" + filename + "\"" ) ;
		}
		
		// free data always starts at byte 20.
		if( really ) {
			stream.seekp( 20, std::ios::beg ) ;
			stream.write( free_data.data(), free_data.size() ) ;
			ui().logger() << "ok.\n" ;
		} else {
			ui().logger() << "ok (dry run; use -really to really make this change).\n" ;
		}
	}
	
	void remove_sample_identifiers(
		std::vector< std::string > const& filenames,
		boost::ptr_vector< std::fstream >& streams,
		bool really
	) {
		assert( filenames.size() == streams.size() ) ;
		for( std::size_t i = 0; i < filenames.size(); ++i ) {
			remove_sample_identifiers( filenames[i], streams[i], really ) ;
		}
	}
	
	void remove_sample_identifiers( std::string const& filename, std::fstream& stream, bool really ) {
		ui().logger() << boost::format( "Checking sample identifiers for \"%s\"..." ) % filename ;
		uint32_t offset ;
		genfile::bgen::Context context ;
		genfile::bgen::read_offset( stream, &offset ) ;
		std::size_t header_size = genfile::bgen::read_header_block( stream, &context ) ;
		
		if( context.flags & genfile::bgen::e_SampleIdentifiers ) {
			ui().logger() << "removing..." ;
			if( really ) {
				std::vector< char > zeros( offset - header_size, 0 ) ;
				// First remove sample IDs flag
				// Flags are last 4 bytes of header.
				stream.seekp( 4 ) ;
				context.flags = context.flags & (~genfile::bgen::e_SampleIdentifiers) ;
				genfile::bgen::write_header_block( stream, context ) ;
				// Now blank out IDs.
				stream.seekp( header_size + 4 ) ;
				stream.write( &zeros[0], zeros.size() ) ;
				ui().logger() << "ok.\n" ;
			} else {
				ui().logger() << "ok (dry run; use -really to really make this change).\n" ;
			}
		} else {
			ui().logger() << "no identifiers present; skipping this file.\n" ;
		}
	}	
} ;

int main( int argc, char** argv ) {
    try {
		EditBgenApplication app( argc, argv ) ;
    }
	catch( appcontext::HaltProgramWithReturnCode const& e ) {
		return e.return_code() ;
	}
	return 0 ;
}
