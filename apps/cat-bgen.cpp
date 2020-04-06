
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
#include "genfile/bgen.hpp"
#include "appcontext/CmdLineOptionProcessor.hpp"
#include "appcontext/ApplicationContext.hpp"
#include "config.h"

namespace globals {
	std::string const program_name = "cat-bgen" ;
	std::string const program_version = bgen_revision ;
}

struct CatBgenOptionProcessor: public appcontext::CmdLineOptionProcessor
{
public:
	std::string get_program_name() const { return globals::program_name ; }

	void declare_options( appcontext::OptionProcessor& options ) {
		// Meta-options
		options.set_help_option( "-help" ) ;

		options.declare_group( "Input / output file options" ) ;
	    options[ "-g" ]
	        .set_description(
				"Path of bgen file(s) to concatenate. "
				"These must all be bgen files containing the same set of samples (in the same order). "
				"They must all be the same bgen version and be stored with the same flags."
			)
			.set_takes_values_until_next_option()
		;

	    options[ "-og" ]
	        .set_description(
				"Path of bgen file to output."
			)
			.set_takes_single_value()
		.set_is_required()
		;

		options[ "-set-free-data" ]
			.set_description(
					"Specify that cat-bgen should set free data in the resulting file to the given string value."
			)
			.set_takes_single_value()
		;

		options[ "-omit-sample-identifier-block" ]
			.set_description(
					"Specify that cat-bgen should omit the sample identifier block in the output, even"
					" if one is present in the first file specified to -og."
			)
		;

	    options[ "-clobber" ]
	        .set_description(
				"Specify that cat-bgen should overwrite existing output file if it exists."
			)
		;
	}
} ;

struct CatBgenApplication: public appcontext::ApplicationContext
{
public:
	CatBgenApplication( int argc, char** argv ):
		appcontext::ApplicationContext(
			globals::program_name,
			globals::program_version,
			std::auto_ptr< appcontext::OptionProcessor >( new CatBgenOptionProcessor ),
			argc,
			argv,
			"-log"
		)
	{
		if( !options().check( "-clobber" ) && boost::filesystem::exists( options().get< std::string >( "-og" ) ) ) {
			ui().logger() << "Output file \"" <<  options().get< std::string >( "-og" ) << "\" exists.  Use -clobber if you want me to overwrite it.\n" ;
			throw appcontext::HaltProgramWithReturnCode( -1 ) ;
		}
		
		std::vector< std::string > inputFilenames = options().get_values< std::string >( "-g" ) ;
		
		if( inputFilenames.size() == 0 ) {
			ui().logger() << "No input files specified; quitting.\n" ;
			throw appcontext::HaltProgramWithReturnCode( -1 ) ;
		}
		
		boost::ptr_vector< std::istream > inputStreams ;
		for( std::size_t i = 0; i < inputFilenames.size(); ++i ) {
			
			inputStreams.push_back(
				std::auto_ptr< std::istream >(
					new std::ifstream( inputFilenames[i], std::ios::binary )
				)
			) ;
		}
		
		std::ofstream outputStream( options().get< std::string > ( "-og" ).c_str(), std::ios::binary ) ;
		genfile::bgen::Context result = concatenate( inputFilenames, inputStreams, outputStream ) ;
		ui().logger() << boost::format( "Finished writing \"%s\" (%d samples, %d variants).\n" )
			% options().get< std::string > ( "-og" ) % result.number_of_samples % result.number_of_variants ;
	}

private:

	genfile::bgen::Context concatenate(
		std::vector< std::string > const& inputFilenames,
		boost::ptr_vector< std::istream >& inputFiles,
		std::ofstream& outputFile
	) const {
		using namespace genfile ;
		assert( inputFiles.size() > 0 ) ;
		assert( inputFilenames.size() == inputFiles.size() ) ;
		// Get some iterators
		std::ostreambuf_iterator< char > outIt( outputFile ) ;
		bgen::Context resultContext ;
		// Deal with the first file, whose header we keep.
		{	
			uint32_t offset = 0 ;
			bgen::read_offset( inputFiles[0], &offset ) ;
			bgen::read_header_block( inputFiles[0], &resultContext ) ;

			ui().logger() << boost::format( "Adding file \"%s\" (%d of %d, %d variants)...\n" )
				% inputFilenames[0] % 1 % inputFiles.size() % resultContext.number_of_variants ;

			if( options().check( "-omit-sample-identifier-block" )) {
				resultContext.flags &= ~genfile::bgen::e_SampleIdentifiers ;
				inputFiles[0].seekg( offset + 4 ) ;
				offset = resultContext.header_size() ;
			}

			if( options().check( "-set-free-data" )) {
				std::string const newFreeData = options().get< std::string >( "-set-free-data" ) ;
				offset += newFreeData.size() - resultContext.free_data.size() ;
				resultContext.free_data = newFreeData ;
			}
			
			// Copy the header 
			bgen::write_offset( outputFile, offset ) ;
			bgen::write_header_block( outputFile, resultContext ) ;

			std::istreambuf_iterator< char > inIt( inputFiles[0] ) ;
			std::istreambuf_iterator< char > endInIt ;

			// Copy everything else
			std::copy( inIt, endInIt, outIt ) ;
		}
		
		for( std::size_t i = 1; i < inputFiles.size(); ++i ) {
			bgen::Context context ;
			uint32_t offset = 0 ;
			bgen::read_offset( inputFiles[i], &offset ) ;
			bgen::read_header_block( inputFiles[i], &context ) ;

			ui().logger() << boost::format( "Adding file \"%s\" (%d of %d, %d variants)...\n" )
				% inputFilenames[i] % (i+1) % inputFiles.size() % context.number_of_variants ;

			if( context.number_of_samples != resultContext.number_of_samples ) {
				ui().logger()
					<< boost::format( "Error: input file #%d ( \"%s\" ) has the wrong number of samples (%d, expected %d).  Quitting.\n" )
						% (i+1) % inputFilenames[i] % context.number_of_samples % resultContext.number_of_samples
				;
				throw appcontext::HaltProgramWithReturnCode( -1 ) ;
			}
			
			if( context.flags != resultContext.flags ) {
				ui().logger()
					<< boost::format( "Error: input file #%d ( \"%s\" ) has the wrong flags (%x, expected %x).  Quitting.\n" )
						% (i+1) % inputFilenames[i] % context.flags % resultContext.flags ;
				throw appcontext::HaltProgramWithReturnCode( -1 ) ;
			}
			
			// Seek forwards to data
			inputFiles[i].seekg( offset + 4 ) ;

			// Copy all the data
			std::istreambuf_iterator< char > inIt( inputFiles[i] ) ;
			std::istreambuf_iterator< char > endInIt ;
			std::copy( inIt, endInIt, outIt ) ;
			
			resultContext.number_of_variants += context.number_of_variants ;
		}
		
		// Finally fix the number of variants in the header.  This starts at byte 8...
		outputFile.seekp( 4 ) ;
		bgen::write_header_block( outputFile, resultContext ) ;
		return resultContext ;
	}

} ;

int main( int argc, char** argv ) {
    try {
		CatBgenApplication app( argc, argv ) ;
    }
	catch( appcontext::HaltProgramWithReturnCode const& e ) {
		return e.return_code() ;
	}
	return 0 ;
}
