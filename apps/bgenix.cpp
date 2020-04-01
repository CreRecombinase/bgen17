
//          Copyright Gavin Band 2008 - 2012.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include <string>
#include <fstream>
#include <vector>
#include <algorithm>
#include <functional>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/tuple/tuple.hpp>
#include <boost/optional.hpp>
#include "appcontext/CmdLineOptionProcessor.hpp"
#include "appcontext/ApplicationContext.hpp"
#include "appcontext/get_current_time_as_string.hpp"
#include "genfile/bgen/bgen.hpp"
#include "genfile/zlib.hpp"
#include "db/Connection.hpp"
#include "db/SQLStatement.hpp"
#include "genfile/bgen/IndexQuery.hpp"
#include "genfile/bgen/View.hpp"
#include "config.h"

namespace bfs = boost::filesystem ;

// #define DEBUG 1

namespace globals {
	std::string const program_name = "bgenix" ;
	std::string const program_version = bgen_version ;
	std::string const program_revision = std::string( bgen_revision ).substr( 0, 7 ) ;
}

struct IndexBgenOptionProcessor: public appcontext::CmdLineOptionProcessor
{
public:
	std::string get_program_name() const { return globals::program_name ; }

	void declare_options( appcontext::OptionProcessor& options ) {
		// Meta-options
		options.set_help_option( "-help" ) ;

		options.declare_group( "Input / output file options" ) ;
		options[ "-g" ]
			.set_description(
				"Path of bgen file to operate on.  (An optional form where \"-g\" is omitted and the filename is specified as the first argument, i.e. bgenix <filename>, can also be used)."
			)
			.set_takes_single_value()
			.set_is_required()
		;
		options[ "-i" ]
			.set_description(
				"Path of index file to use. If not specified, " + globals::program_name + " will look for an index file of the form '<filename>.bgen.bgi' "
				" where '<filename>.bgen' is the bgen file name specified by the -g option."
			)
			.set_takes_single_value()
		;

		options[ "-table" ]
			.set_description( "Specify the table (or view) that bgenix should read the file index from. "
				"This only affects reading the index file.  The named table or view should have the"
				" same schema as the Variant table written by bgenix on index creation."
			)
			.set_takes_single_value()
			.set_default_value( "Variant" )
		;
		
		options.declare_group( "Indexing options" ) ;
		options[ "-index" ]
			.set_description( "Specify that bgenix should build an index for the BGEN file"
				" specified by the -g option."
			)
		;
		options[ "-clobber" ]
			.set_description(
				"Specify that bgenix should overwrite existing index file if it exists."
			)
		;
		options[ "-with-rowid" ]
			.set_description( "Create an index file that does not use the 'WITHOUT ROWID' feature."
				" These are suitable for use with sqlite versions < 3.8.2, but may be less efficient." ) ;
		

		options.declare_group( "Variant selection options" ) ;
		options[ "-incl-range" ]
			.set_description(
				"Include variants in the specified genomic interval in the output. "
				"(If the argument is the name of a valid readable file, the file will "
				"be opened and whitespace-separated rsids read from it instead.)"
				" Each interval must be of the form <chr>:<pos1>-<pos2> where <chr> is a chromosome identifier "
				" and pos1 and pos2 are positions with pos2 >= pos1. "
				" One of pos1 and pos2 can also be omitted, in which case the range extends to the start or"
				" end of the chromosome as appropriate. "
				" Position ranges are treated as closed (i.e. <pos1> and <pos2> are included in the range)."
				"If this is specified multiple times, variants in any of the specified ranges will be included."
			)
			.set_takes_values_until_next_option()
		;

		options[ "-excl-range" ]
			.set_description(
				"Exclude variants in the specified genomic interval from the output. "
				"See the description of -incl-range for details."
				"If this is specified multiple times, variants in any of the specified ranges will be excluded."
			)
			.set_takes_values_until_next_option()
		;

		options[ "-incl-rsids" ]
			.set_description(
				"Include variants with the specified rsid(s) in the output. "
				"If the argument is the name of a valid readable file, the file will "
				"be opened and whitespace-separated rsids read from it instead."
				"If this is specified multiple times, variants with any of the specified ids will be included."
			)
			.set_takes_values_until_next_option()
		;

		options[ "-excl-rsids" ]
			.set_description(
				"Exclude variants with the specified rsid(s) from the output. "
				"See the description of -incl-range for details."
					"If this is specified multiple times, variants with any of the specified ids will be excluded."
			)
			.set_takes_values_until_next_option()
		;
		
		options.declare_group( "Output options" ) ;
		options[ "-list" ]
			.set_description( "Suppress BGEN output; instead output a list of variants." ) ;
		options[ "-v11" ]
			.set_description(
				"Transcode to BGEN v1.1 format.  (Currently, this is only supported if the input"
				" is in BGEN v1.2 format with 8 bits per probability, all samples are diploid,"
				" and all variants biallelic)."
			) ;
		options[ "-compression-level" ]
			.set_description(
				"Zlib compression level to use when transcoding to BGEN v1.1 format."
			)
			.set_takes_single_value()
			.set_default_value( 9 ) ;
		options[ "-vcf" ]
			.set_description(
				"Transcode to VCF format.  VCFs will have GP field (or 'HP' field for phased data), and a GT field inferred from the probabilities by threshholding."
			) ;

		// Option interdependencies
		options.option_excludes_group( "-index", "Variant selection options" ) ;
		options.option_excludes_group( "-index", "Output options" ) ;
		options.option_excludes_option( "-list", "-v11" ) ;
		options.option_excludes_option( "-vcf", "-list" ) ;
		options.option_excludes_option( "-vcf", "-v11" ) ;
		options.option_implies_option( "-clobber", "-index" ) ;
		options.option_implies_option( "-compression-level", "-v11" ) ;
	}
} ;

typedef uint8_t byte_t ;

// Verify that the given index matches this file using the supplied metadata.
// Throw std::invalid_argument error if they mismatch, otherwise return the index.
void check_metadata(
	genfile::bgen::IndexQuery::FileMetadata const& file,
	boost::optional< genfile::bgen::IndexQuery::FileMetadata > const& index
) {
	if( index ) {
		if( file.size != (*index).size ) {
			std::string const message = "!! Size of file \"" + file.filename + "\" ("
				+ std::to_string( file.size ) + " bytes)"
				+ " differs from that recorded in the index file ("
				+ std::to_string( (*index).size ) + " bytes).\n"
				+ "Do you need to recreate the index?" ;
			throw std::invalid_argument( message ) ;
			//throw appcontext::HaltProgramWithReturnCode( -1 ) ;
		}

		if( file.first_bytes != (*index).first_bytes ) {
			std::string const message = "!! File \"" + file.filename + "\" has different initial bytes"
				+ " than recorded in the index file \"" + (*index).filename + "\" - that can't be right.\n"
				+ "Do you need to recreate the index?" ;
			throw std::invalid_argument( message ) ;
			//throw appcontext::HaltProgramWithReturnCode( -1 ) ;
		}
	}
}

/* IndexBgenApplication */
struct IndexBgenApplication: public appcontext::ApplicationContext
{
public:
	IndexBgenApplication( int argc, char** argv ):
		appcontext::ApplicationContext(
			globals::program_name,
			globals::program_version + ", revision " + globals::program_revision,
			std::auto_ptr< appcontext::OptionProcessor >( new IndexBgenOptionProcessor ),
			argc,
			argv,
			"-log"
		)
	{
		setup() ;
	}

private:
	std::string m_bgen_filename ;
	std::string m_index_filename ;

private:
	void setup() {
		m_bgen_filename = options().get< std::string >( "-g" ) ;
		m_index_filename = options().check( "-i" ) ? options().get< std::string > ( "-i" ) : (m_bgen_filename + ".bgi") ;
		if( !bfs::exists( m_bgen_filename )) {
			ui().logger() << "!! Error, the BGEN file \"" << m_bgen_filename << "\" does not exist!\n" ;
			throw std::invalid_argument( m_bgen_filename ) ;
		}
		if( options().check( "-index" )) {
			if( bfs::exists( m_index_filename ) && !options().check( "-clobber" )) {
				ui().logger() << "!! Error, the index file \"" + m_index_filename + "\" already exists, use -clobber if you want to overwrite it.\n" ;
				throw appcontext::HaltProgramWithReturnCode( -1 ) ;
			} else {
				create_bgen_index( m_bgen_filename, m_index_filename ) ;
			}
		} else {
			process_selection( m_bgen_filename, m_index_filename ) ;
		}
	}

	void create_bgen_index( std::string const& bgen_filename, std::string const& index_filename ) {
		try {
			create_bgen_index_unsafe( bgen_filename, index_filename ) ;
		}
		catch( std::exception const& e ) {
			ui().logger() << "\n!! " << e.what() << "\n" ;
			throw appcontext::HaltProgramWithReturnCode( -1 ) ;
		}
	}

	void create_bgen_index_unsafe( std::string const& bgen_filename, std::string const& index_filename ) {
		db::Connection::UniquePtr result ;
		ui().logger()
			<< boost::format( "%s: creating index for \"%s\" in \"%s\"...\n" ) % globals::program_name % bgen_filename % index_filename ;

		if( bfs::exists( index_filename + ".tmp" ) && !options().check( "-clobber" ) ) {
			throw std::invalid_argument( "Error: an incomplete index file \"" + (index_filename + ".tmp") + "\" already exists.\n"
				"This probably reflects a previous bgenix run that was terminated.\n"
				"Please delete the file (or use -clobber to overwrite it automatically).\n"
			) ;
		}

		try {
			result = create_bgen_index_direct( bgen_filename, index_filename + ".tmp" ) ;
			bfs::rename( index_filename + ".tmp", index_filename ) ;
		} catch( db::StatementStepError const& e ) {
			ui().logger() << "!! Error in \"" << e.spec() << "\": " << e.description() << ".\n" ;
			bfs::remove( index_filename + ".tmp" ) ;
			throw appcontext::HaltProgramWithReturnCode( -1 ) ;
		} catch( ... ) {
			// Remove the incomplete attempt at an index file.
			bfs::remove( index_filename + ".tmp" ) ;
			throw ;
		}
	}
	
	db::Connection::UniquePtr create_bgen_index_direct( std::string const& bgen_filename, std::string const& index_filename ) {
		db::Connection::UniquePtr connection = db::Connection::create( "file:" + index_filename + "?nolock=1", "rw" ) ;

		connection->run_statement( "PRAGMA locking_mode = EXCLUSIVE ;" ) ;
		connection->run_statement( "PRAGMA journal_mode = MEMORY ;" ) ;
		connection->run_statement( "PRAGMA synchronous = OFF;" ) ;
		
		db::Connection::ScopedTransactionPtr transaction = connection->open_transaction( 240 ) ;
		setup_index_file( *connection ) ;
		// Close and open the transaction
		transaction.reset() ;

		db::Connection::StatementPtr insert_metadata_stmt = connection->get_statement(
			"INSERT INTO Metadata( filename, file_size, last_write_time, first_1000_bytes, index_creation_time ) VALUES( ?, ?, ?, ?, ? )"
		) ;

		db::Connection::StatementPtr insert_variant_stmt = connection->get_statement(
			"INSERT INTO Variant( chromosome, position, rsid, number_of_alleles, allele1, allele2, file_start_position, size_in_bytes ) "
			"VALUES( ?, ?, ?, ?, ?, ?, ?, ? )"
		) ;

		genfile::bgen::View bgenView( bgen_filename ) ;
		
		insert_metadata_stmt
			->bind( 1, bgen_filename )
			.bind( 2, bgenView.file_metadata().size )
			.bind( 3, uint64_t( bgenView.file_metadata().last_write_time ) )
			.bind( 4, &bgenView.file_metadata().first_bytes[0], &bgenView.file_metadata().first_bytes[0] + bgenView.file_metadata().first_bytes.size() )
			.bind( 5, appcontext::get_current_time_as_string() )
			.step() ;

		ui().logger()
			<< boost::format( "%s: Opened \"%s\" with %d variants...\n" ) % globals::program_name % bgen_filename % bgenView.number_of_variants() ;
		
		std::string chromosome, rsid, SNPID ;
		uint32_t position ;
		std::vector< std::string > alleles ;
		alleles.reserve(100) ;
		std::size_t const chunk_size = 10 ;
		
		transaction = connection->open_transaction( 240 ) ;
		
		{
			auto progress_context = ui().get_progress_context( "Building BGEN index" ) ;
			std::size_t variant_count = 0;
			int64_t file_pos = int64_t( bgenView.current_file_position() ) ;
			try {
				while( bgenView.read_variant( &SNPID, &rsid, &chromosome, &position, &alleles ) ) {
#if DEBUG
					std::cerr << "read variant:" << chromosome << " " << position << " " << rsid << " " << file_pos << " " << alleles.size() << ".\n" << std::flush ;
					std::cerr << "alleles: " << alleles[0] << ", "  << alleles[1] << ".\n" << std::flush ;
#endif
					bgenView.ignore_genotype_data_block() ;
					int64_t file_end_pos = int64_t( bgenView.current_file_position() ) ;
					assert( alleles.size() > 1 ) ;
					assert( (file_end_pos - file_pos) > 0 ) ;
					insert_variant_stmt
						->bind( 1, chromosome )
						.bind( 2, position )
						.bind( 3, rsid )
						.bind( 4, int64_t( alleles.size() ) )
						.bind( 5, alleles[0] )
						.bind( 6, alleles[1] )
						.bind( 7, file_pos )
						.bind( 8, file_end_pos - file_pos )
						.step()
					;
					insert_variant_stmt->reset() ;
				
					progress_context( ++variant_count, bgenView.number_of_variants() ) ;
			
				// Make sure and commit every 10000 SNPs.
					if( variant_count % chunk_size == 0 ) {
		//				ui().logger()
		//					<< boost::format( "%s: Writing variants %d-%d...\n" ) % bgenView.number_of_variants() % (variant_count-chunk_size) % (variant_count-1) ;
					
						transaction.reset() ;
						transaction = connection->open_transaction( 240 ) ;
					}
					file_pos = file_end_pos ;
#if DEBUG
					std::cerr << "Record inserted.\n" << std::flush ;
#endif
								}
			}
			catch( genfile::bgen::BGenError const& e ) {
				ui().logger() << "!! (" << e.what() << "): an error occurred reading from the input file.\n" ;
				ui().logger() << "Last observed variant was \"" << SNPID.substr(0,10) << "\", \"" << rsid.substr(0,10) << "\"...\n" ;
				ui().logger() << "Reached byte " << file_pos << " in input file, which has size " << bgenView.file_metadata().size << ".\n" ;
				throw ;
			}
 			catch( db::StatementStepError const& e ) {
				ui().logger() << "Last observed variant was " << SNPID << " " << rsid << " " << chromosome << " " << position ;
				for( std::size_t i = 0; i < alleles.size(); ++i ) {
					ui().logger() << " " << alleles[i] ;
				}
				ui().logger() << "\n" ;
				ui().logger() << "Reached byte " << file_pos << " in input file, which has size " << bgenView.file_metadata().size << ".\n" ;
				throw ;
			}
		}
		return connection ;
	}
	
	void setup_index_file( db::Connection& connection ) {
		std::string const tag = options().check( "-with-rowid" ) ? "" : " WITHOUT ROWID" ;

		connection.run_statement(
			"CREATE TABLE Metadata ("
			" filename TEXT NOT NULL,"
			" file_size INT NOT NULL,"
			" last_write_time INT NOT NULL,"
			" first_1000_bytes BLOB NOT NULL,"
			" index_creation_time INT NOT NULL"
			")"
		) ;

		connection.run_statement(
			"CREATE TABLE Variant ("
			"  chromosome TEXT NOT NULL,"
			"  position INT NOT NULL,"
			"  rsid TEXT NOT NULL,"
			"  number_of_alleles INT NOT NULL,"
			"  allele1 TEXT NOT NULL,"
			"  allele2 TEXT NULL,"
			"  file_start_position INT NOT NULL," // 
			"  size_in_bytes INT NOT NULL,"       // We put these first to minimise cost of retrieval
			"  PRIMARY KEY (chromosome, position, rsid, allele1, allele2, file_start_position )"
			")" + tag
		) ;
	}
	
	void process_selection( std::string const& bgen_filename, std::string const& index_filename ) const {
		try {
			process_selection_unsafe( bgen_filename, index_filename ) ;
		}
		catch( std::invalid_argument const& e ) {
			std::cerr << e.what() << "\n" ;
			throw appcontext::HaltProgramWithReturnCode( -1 ) ;
		}
		catch( ... ) {
			throw ;
		}
	}

	void process_selection_unsafe( std::string const& bgen_filename, std::string const& index_filename ) const {
		genfile::bgen::View bgenView( bgen_filename ) ;
		genfile::bgen::IndexQuery::UniquePtr query = create_index_query( index_filename ) ;

		//setup_query( *index ) ;
		bool const transcode
			= options().check( "-list" )
			|| options().check( "-vcf" )
			|| options().check( "-v11" ) ;

		if( transcode ) {
			bgenView.set_query( query ) ;
			if( options().check( "-list" ) ) {
				process_selection_list( bgenView ) ;
			} else if( options().check( "-vcf" )) {
				process_selection_transcode( bgenView, "vcf" ) ;
			} else if( options().check( "-v11" )) {
				process_selection_transcode( bgenView, "bgen_v1.1" ) ;
			}
		} else {
			// When not transcoding we skip BgenParser and use the bgen file directly.
			check_metadata( bgenView.file_metadata(), query->file_metadata() ) ;
			process_selection_notranscode( bgen_filename, query ) ;
		}
	}

	genfile::bgen::IndexQuery::UniquePtr create_index_query( std::string const& filename ) const {
		genfile::bgen::SqliteIndexQuery::UniquePtr query ;
		try {
			query.reset( new genfile::bgen::SqliteIndexQuery( filename, options().get< std::string >( "-table" )) ) ;
		} catch( std::invalid_argument const& e ) {
			std::cerr << "!! Error opening index file \"" << filename
				<< "\": " << e.what() << "\n" ;
			std::cerr << "Use \"bgenix -g " + options().get< std::string >( "-g" ) + " -index\" to create the index file.\n" ;
			throw appcontext::HaltProgramWithReturnCode( -1 ) ;
		}
	
		if( options().check( "-incl-range" )) {
			auto const elts = collect_unique_ids( options().get_values< std::string >( "-incl-range" ));
			for( std::string const& elt: elts ) {
				query->include_range( parse_range( elt )) ;
			}
		}
		if( options().check( "-excl-range" )) {
			auto const elts = collect_unique_ids( options().get_values< std::string >( "-excl-range" ));
			for( std::string const& elt: elts ) {
				query->exclude_range( parse_range( elt )) ;
			}
		}
		if( options().check( "-incl-rsids" )) {
			auto const ids = collect_unique_ids( options().get_values< std::string >( "-incl-rsids" ));
			query->include_rsids( ids ) ;
		}

		if( options().check( "-excl-rsids" )) {
			auto const ids = collect_unique_ids( options().get_values< std::string >( "-excl-rsids" ));
			query->exclude_rsids( ids ) ;
		}

		{
			auto progress_context = ui().get_progress_context( "Building query" ) ;
			query->initialise( progress_context ) ;
		}
		return( genfile::bgen::IndexQuery::UniquePtr( query.release() )) ; // Using std::auto_ptr so we need these gymnastics
	}
	
	std::vector< std::string > collect_unique_ids( std::vector< std::string > const& ids_or_filenames ) const {
		std::vector< std::string > result ;
		for( auto elt: ids_or_filenames ) {
			if( bfs::exists( elt )) {
				std::ifstream f( elt ) ;
				std::copy(
					std::istream_iterator< std::string >( f ),
					std::istream_iterator< std::string >(),
					std::back_inserter< std::vector< std::string > >( result )
				) ;
			} else {
				result.push_back( elt ) ;
			}
		}
		// now sort and uniqueify them...
		std::sort( result.begin(), result.end() ) ;
		std::vector< std::string >::const_iterator newBack = std::unique( result.begin(), result.end() ) ;
		result.resize( newBack - result.begin() ) ;
		return result ;
	}

	void process_selection_notranscode( std::string const& bgen_filename, genfile::bgen::IndexQuery::UniquePtr index ) const {
		std::ifstream bgen_file( bgen_filename, std::ios::binary ) ;
		uint32_t offset = 0 ;

		using namespace genfile ;
		bgen::Context context ;
		bgen::read_offset( bgen_file, &offset ) ;
		bgen::read_header_block( bgen_file, &context ) ;

		// Write the new context after adjusting the variant count.
		context.number_of_variants = index->number_of_variants() ;
		bgen::write_offset( std::cout, offset ) ;
		bgen::write_header_block( std::cout, context ) ;
		std::istreambuf_iterator< char > inIt( bgen_file ) ;
		std::istreambuf_iterator< char > endInIt ;
		std::ostreambuf_iterator< char > outIt( std::cout ) ;

		// Copy everything else up to the start of the data
		std::copy_n( inIt, offset - context.header_size(), outIt ) ;

		{
			auto progress_context = ui().get_progress_context( "Processing " + std::to_string( index->number_of_variants() ) + " variants" ) ;
			// Now we go for it
			for( std::size_t i = 0; i < index->number_of_variants(); ++i ) {
				std::pair< int64_t, int64_t> range = index->locate_variant( i ) ;
				bgen_file.seekg( range.first ) ;
				std::istreambuf_iterator< char > inIt( bgen_file ) ;
				std::copy_n( inIt, range.second, outIt ) ;
				progress_context( i+1, index->number_of_variants() ) ;
			}
		}
		std::cerr << boost::format( "%s: wrote data for %d variants to stdout.\n" ) % globals::program_name % index->number_of_variants() ;
	}
	
	void process_selection_list( genfile::bgen::View& bgenView ) const {
		std::cout << boost::format( "# %s: started %s\n" ) % globals::program_name % appcontext::get_current_time_as_string() ;
		std::cout << "alternate_ids\trsid\tchromosome\tposition\tnumber_of_alleles\tfirst_allele\talternative_alleles\n" ;
		
		std::string SNPID, rsid, chromosome ;
		uint32_t position ;
		std::vector< std::string > alleles ;

		for( std::size_t i = 0; i < bgenView.number_of_variants(); ++i ) {
			bool success = bgenView.read_variant(
				&SNPID, &rsid, &chromosome, &position, &alleles
			) ;
			if( SNPID.empty() ) {
				SNPID = "." ;
			}
			if( rsid.empty() ) {
				rsid = "." ;
			}
			std::cout << SNPID << "\t" << rsid << "\t" << chromosome << "\t" << position << "\t" << alleles.size() << "\t" << alleles[0] << "\t" ;
			for( std::size_t allele_i = 1; allele_i < alleles.size(); ++allele_i ) {
				std::cout << (( allele_i > 1 ) ? "," : "" ) << alleles[allele_i] ;
			}
			std::cout << "\n" ;
			if( !success ) {
				throw std::invalid_argument( "positions" ) ;
			}
			bgenView.ignore_genotype_data_block() ;
		}
		std::cout << boost::format( "# %s: success, total %d variants.\n" ) % globals::program_name % bgenView.number_of_variants() ;
	}

	void process_selection_transcode(
		genfile::bgen::View& view,
		std::string const& format
	) const {
		if( format == "vcf" ) {
			process_selection_transcode_bgen_vcf( view ) ;
		} else if( format == "bgen_v1.1" ) {
			process_selection_transcode_bgen_v11( view ) ;
		} else {
			throw std::invalid_argument( "format=\"" + format + "\"" ) ;
		}
	}

	struct VCFProbWriter {
		VCFProbWriter( std::ostream& out ):
			m_out( out )
		{}
		
		// Called once allowing us to set storage.
		void initialise( std::size_t number_of_samples, std::size_t number_of_alleles ) {
			// Nothing to do.
			m_number_of_alleles = number_of_alleles ;
		}
	
		// If present with this signature, called once after initialise()
		// to set the minimum and maximum ploidy and numbers of probabilities among samples in the data.
		// This enables us to set up storage for the data ahead of time.
		void set_min_max_ploidy( uint32_t min_ploidy, uint32_t max_ploidy, uint32_t min_entries, uint32_t max_entries ) {
			// Make sure we've enough space.
			m_data.reserve( max_entries ) ;
		}
	
		// Called once per sample to determine whether we want data for this sample
		bool set_sample( std::size_t i ) {
			// Yes, here we want info for all samples.
			return true ;
		}
	
		// Called once per sample to set the number of probabilities that are present.
		void set_number_of_entries(
			std::size_t ploidy,
			std::size_t number_of_entries,
			genfile::OrderType order_type,
			genfile::ValueType value_type
		) {
			assert( value_type == genfile::eProbability ) ;
			m_data.resize( number_of_entries ) ;
			m_out << "\t" ;
			m_ploidy = ploidy ;
			m_order_type = order_type ;
			m_missing = false ;
		}

		void set_value( uint32_t entry_i, double value ) {
			m_data[ entry_i ] = value ;
			if( entry_i == m_data.size() - 1 ) {
				write_sample_entry() ;
			}
		}

		void set_value( uint32_t entry_i, genfile::MissingValue value ) {
			m_data[ entry_i ] = -1 ;
			m_missing = true ;
			if( entry_i == m_data.size() - 1 ) {
				write_sample_entry() ;
			}
		}

		void finalise() {
			m_out << "\n" ;
		}

	private:
		std::ostream& m_out ;
		std::vector< double > m_data ;
		std::size_t m_number_of_alleles ;

		// Used to keep track of what we're doing.
		std::size_t m_ploidy ;
		genfile::OrderType m_order_type ;
		bool m_missing ;
		
		// These fields are used to enumerate genotypes for the GT field.
		std::vector< uint16_t > m_genotype_allele_limits ;
		std::vector< uint16_t > m_genotype ;
		
		// space to assemble GT field.
		std::string m_GT_buffer ;
		void write_sample_entry() {
			if( m_missing ) {
				std::string const GT_separator = (m_order_type == genfile::ePerPhasedHaplotypePerAllele) ? "|" : "/" ;
				for( uint32_t i = 0 ; i < m_ploidy; ++i ) {
					m_out << ((i>0) ? GT_separator : "" )
						<< "." ;
				}
				m_out << ":" ;
			} else {
				std::string const& GT = construct_GT( m_data, 0.9 ) ;
				m_out << GT
					<< ":" ;
			}
			for( std::size_t i = 0; i < m_data.size(); ++i ) {
				m_out << ((i>0) ? "," : "") ;
				if( m_data[i] == -1 ) {
					m_out << "." ;
				} else {
					m_out << m_data[i] ;
				}
			}
		}

		std::string const& construct_GT( std::vector< double > const& probs, double const threshhold ) {
			if( m_order_type == genfile::ePerPhasedHaplotypePerAllele ) {
				return construct_phased_GT( probs, threshhold ) ;
			} else {
				return construct_unphased_GT( probs, threshhold ) ;
			}
		}

		std::string const& construct_phased_GT( std::vector< double > const& probs, double const threshhold ) {
			m_GT_buffer.clear() ;
			// for phased data it is simple:
			for( uint32_t i = 0; i < m_ploidy; ++i ) {
				uint32_t j = 0 ;
				for( ; j < m_number_of_alleles; ++j ) {
					if( probs[i*m_number_of_alleles+j] > threshhold ) {
						break ;
					}
				}
				if( j < m_number_of_alleles ) {
					m_GT_buffer += std::to_string( j ) + "|" ;
				} else {
					m_GT_buffer += ".|" ;
				}
			}
			// remove trailing separator
			m_GT_buffer.resize( m_GT_buffer.size() - 1 ) ;
			return m_GT_buffer ;
		}

		std::string const& construct_unphased_GT( std::vector< double > const& probs, double const threshhold ) {
			// To construct the GT field for unphased data, we must enumerate
			// all of the possible genotypes.
			// These come in colex order of the the allele count representation.
			// Specifically, we have m_ploidy = n chromosomes in total.
			// Genotypes are all ways to put n_alleles = k alleles into those chromosomes.
			// We represent these as k-vectors that sum to n (i.e. v=(v_i) where v_i is the count of allele i),
			// or equivalently, (k-1)-vectors that sum to at most n.
			// Colex order is lexicographical order of these vectors, reading them right-to-left.
			// E.g. for ploidy = 3 and 3 alleles, the order is
			// 3,0,0 = AAA
			// 2,1,0 = AAB
			// 1,2,0 = ABB
			// 0,3,0 = BBB
			// 2,0,1 = AAC
			// 1,1,1 = ABC
			// 0,2,1 = BBC
			// 1,0,2 = ACC
			// 0,1,2 = BCC
			// 0,0,3 = CCC
			// Here we enumerate these and bail out when we hit a probability over the threshhold.
			m_genotype_allele_limits.assign( (m_number_of_alleles-1), m_ploidy ) ;
			m_genotype.assign( m_number_of_alleles, 0 ) ;
			// Set up first genotype - all ref allele
			m_genotype[0] = m_ploidy ;

			// Iterate through genotypes.
			bool metThreshhold = false ;
			for( std::size_t index = 0; true; ++index ) {
				if( probs[index] > threshhold ) {
					metThreshhold = true ;
					break ;
				}
				
				// Move to next possible genotype
				std::size_t j = 0 ;
				for( ; j < (m_number_of_alleles-1); ++j ) {
					uint16_t value = m_genotype[j+1] ;
					if( value < m_genotype_allele_limits[ j ] ) {
						++m_genotype[j+1] ;
						--m_genotype[0] ;
						for( std::size_t k = 0; k < j; ++k ) {
							--m_genotype_allele_limits[k] ;
						}
						break ;
					} else {
						// allele count has reached its limit.
						// Reset it to zero.
						// Note that to get here all lower-order counts must be zero.
						m_genotype[j+1] = 0 ;
						m_genotype[0] += value ;
						for( std::size_t k = 0; k < j; ++k ) {
							m_genotype_allele_limits[k] += value ;
						}
					}
				}
				if( j == (m_number_of_alleles-1) ) {
					break ;
				}
			}
			
			m_GT_buffer.clear() ;
			if( metThreshhold ) {
				for( std::size_t allele = 0; allele < m_number_of_alleles; ++allele ) {
					std::string const elt = std::to_string( allele ) + "/" ;
					for( uint16_t count = 0; count < m_genotype[allele]; ++count ) {
						m_GT_buffer += elt ;
					}
				}
			} else {
				for( std::size_t i = 0; i < m_ploidy; ++i ) {
					m_GT_buffer += "./" ;
				}
			}
			// remove trailing slash.
			m_GT_buffer.resize( m_GT_buffer.size() - 1 ) ;
			return m_GT_buffer ;
		}
	} ;

	void process_selection_transcode_bgen_vcf(
		genfile::bgen::View& bgenView
	) const {
		uint32_t const inputLayout = bgenView.context().flags & genfile::bgen::e_Layout ;

		std::cout << "##fileformat=VCFv4.2\n"
			<< "##FORMAT=<ID=GT,Type=String,Number=1,Description=\"Threshholded genotype call\">\n"
			<< "##FORMAT=<ID=GP,Type=Float,Number=G,Description=\"Genotype call probabilities\">\n"
			<< "##FORMAT=<ID=HP,Type=Float,Number=.,Description=\"Haplotype call probabilities\">\n"
			<< "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT" ;
		
		bgenView.get_sample_ids(
			[]( std::string const& name ) { std::cout << "\t" << name ; }
		) ;
		
		std::cout << "\n" ;
		std::string SNPID, rsid, chromosome ;
		uint32_t position ;
		std::vector< std::string > alleles ;

		// Map from bit sizes to vcf encoding tables
		typedef std::map< std::size_t, std::pair< std::size_t, std::string > > EncodingTables ;
		EncodingTables encoding_tables ;
		std::vector< char > buffer ;
	
		{
			auto progress_context = ui().get_progress_context( "Processing " + std::to_string( bgenView.number_of_variants() ) + " variants" ) ;
			for( std::size_t i = 0; i < bgenView.number_of_variants(); ++i ) {
				bool success = bgenView.read_variant(
					&SNPID, &rsid, &chromosome, &position, &alleles
				) ;
				assert( success ) ;
				assert( alleles.size() > 1 ) ;
				std::cout << chromosome
					<< "\t" << position
					<< "\t" << rsid << ( (SNPID == rsid) ? "" : (";" + SNPID))
					<< "\t" << alleles[0]
					<< "\t" ;
				for( std::size_t j = 1; j < alleles.size(); ++j ) {
					std::cout << ((j==1)?"":",") << alleles[j] ;
				}
				std::cout
					<< "\t"
					<< ".\t" // QUAL
					<< ".\t" // FILTER
					<< ".\t" // INFO
					<< "GT:GP" // FORMAT
				;

				if( inputLayout == genfile::bgen::e_Layout2 ) {
					// Inspect data and use faster method if available
					// Currently this works for 1, 2, 4 or 8-bit encoded data.
					genfile::bgen::v12::GenotypeDataBlock pack ;
					bgenView.read_and_unpack_v12_genotype_data_block( &pack ) ;
					if( (pack.bits == 1 || pack.bits == 2 || pack.bits == 4 || pack.bits == 8 ) && pack.ploidyExtent[0] == 2 && pack.ploidyExtent[1] == 2 && pack.phased == false ) {
						typedef std::pair< std::string::const_iterator, std::string::const_iterator > EncodedRange ;
						// First is the size of each entry, second is the data itself.
						typedef std::pair< std::size_t, std::string > VcfEncodingTable ;
						VcfEncodingTable const& vcf_encoding_table = get_vcf_encoding_table( encoding_tables, pack.bits ) ;
						buffer.resize( pack.numberOfSamples * (1+vcf_encoding_table.first) + 1 ) ;
						char* buffer_p = &buffer[0] ;
						for( std::size_t i = 0; i < pack.numberOfSamples; ++i ) {
							if( pack.ploidy[i] & 0x80 ) {
								*buffer_p++ = '\t' ;
								*buffer_p++ = '.' ;
								*buffer_p++ = '/' ;
								*buffer_p++ = '.' ;
							} else {
								// Find bytes encoding this sample
								std::size_t genotype = extract_encoded_genotype( pack.buffer, pack.end, i, pack.bits ) ;
								EncodedRange const& encoding = extract_vcf_encoding( vcf_encoding_table, genotype ) ;
								*buffer_p++ = '\t' ;
								buffer_p = std::copy( encoding.first, encoding.second, buffer_p ) ;
							}
						}
						*buffer_p++ = '\n' ;
						
						std::cout.write( &buffer[0], (buffer_p - &buffer[0]) ) ;
					} else {
						VCFProbWriter writer( std::cout ) ;
						genfile::bgen::v12::parse_probability_data( pack, writer ) ;
					}
				} else {
					// Use generic, possibly slow method
					VCFProbWriter writer( std::cout ) ;
					bgenView.read_genotype_data_block( writer ) ;
				}
				progress_context( i+1, bgenView.number_of_variants() ) ;
			}
		}
	}

	typedef std::map< std::size_t, std::pair< std::size_t, std::string > > EncodingTables ;
	std::pair< std::size_t, std::string > const& get_vcf_encoding_table( EncodingTables& encoding_tables, int bits ) const {
		EncodingTables::const_iterator table_i = encoding_tables.find( bits ) ;
		if( table_i == encoding_tables.end() ) {
			std::pair< EncodingTables::const_iterator, bool > inserted = encoding_tables.insert( std::make_pair( bits, compute_vcf_encoding_table( bits )) ) ;
			assert( inserted.second ) ;
			table_i = inserted.first ;
		}
		std::pair< std::size_t, std::string > const& result = table_i->second ;
		return result ;
	}

	std::pair< std::string::const_iterator, std::string::const_iterator > extract_vcf_encoding(
		std::pair< std::size_t, std::string > const& vcf_encoding_table,
		std::size_t genotype 
	) const {
		return std::make_pair(
			vcf_encoding_table.second.begin() + genotype*vcf_encoding_table.first, 
			vcf_encoding_table.second.begin() + (genotype+1)*vcf_encoding_table.first
		) ;
	}

	uint16_t extract_encoded_genotype(
		byte_t const* buffer,
		byte_t const* end,
		std::size_t i,
		int const bits
	) const {
		uint16_t const* encoding_p = reinterpret_cast< uint16_t const* >( buffer + (2*i*bits) / 8 ) ;
		uint16_t const encodingMask = uint16_t( 0xFFFFFF ) >> (16 - (2 * bits)) ;
		int encodingShift = 
			(bits >= 4 )
				? 0 
				: (2*bits) * (i % (4 / bits)) ;
		return ((*encoding_p) >> encodingShift ) & encodingMask ;
	}
	
	std::pair< std::size_t, std::string > compute_vcf_encoding_table( int const bits ) const {
		assert( bits == 1 || bits == 2 || bits == 4 || bits == 8 ) ;
		int dps = 0 ;
		switch( bits ) {
			case 1: dps = 0; break ;
			case 2: dps = 2; break ;
			case 4: dps = 3; break ;
			case 8: dps = 4; break ;
		}
		int const valueSize = 3 + 3 + 3*(dps+((dps>0)?2:1)) ;
		std::size_t const numberOfDistinctProbs = 1 << bits ;
		uint16_t const maxProb = numberOfDistinctProbs - 1 ;

		std::ostringstream formatter ;
		formatter << std::fixed << std::setprecision( dps ) ;
		// For 8 bit encoding, probs are to 3 dps i.e. x.xxx, gt is ./. 
		// max length of a field is 3 + (3*5) + 3 = 21.
		std::string storage( valueSize * numberOfDistinctProbs * numberOfDistinctProbs, ' ' ) ;
		std::string gt ;
		for( uint16_t x = 0; x <= maxProb; ++x ) {
			for( uint16_t y = 0; y <= maxProb-x; ++y ) {
				formatter.str("") ;
				uint16_t z = maxProb-x-y ;
				uint16_t key = (y<<bits) | x ;
				double const p0 = x/double(maxProb) ;
				double const p1 = y/double(maxProb) ;
				double const p2 = z/double(maxProb) ;
				if( p0 > 0.9 ) {
					gt = "0/0" ;
				} else if( p1 > 0.9 ) {
					gt = "0/1" ;
				} else if( p2 > 0.9 ) {
					gt = "1/1" ;
				} else {
					gt = "./." ;
				}
				std::size_t storageIndex = key * valueSize ;
				formatter << gt << ":" << p0 << "," << p1 << "," << p2 ; 
				std::string const value = formatter.str() ;
				assert( value.size() == valueSize ) ;
				std::copy( value.begin(), value.end(), &storage[0] + storageIndex ) ;
			}
		}
		return std::make_pair( valueSize, storage ) ;
	}
	
	// This function implements an efficient transcode from a specific type of BGEN file
	// To BGEN v1.1 files.
	// Specifically we support BGEN 'layout=2' files (BGEN v1.2 and above) with 8 bits per
	// probability.
	// For efficiency, instead of a full parse we extract encoded data and use a lookup table
	// to generate BGEN v1.1 values for encoding.
	void process_selection_transcode_bgen_v11(
		genfile::bgen::View& bgenView
	) const {
		// Currently this is only supported in a very restricted scenario.
		// Namely, when the format is BGEN v1.2, unphased data, encoded
		// with 8 bits per probability, converting to a BGEN v1.1 file.
		// And all variants must be biallelic.
		uint32_t const inputLayout = bgenView.context().flags & genfile::bgen::e_Layout ;
		if( inputLayout != genfile::bgen::e_Layout2 ) {
			throw std::invalid_argument( "bgen_filename=\"" + bgenView.file_metadata().filename + "\"" ) ;
		}

		// specify flags for BGEN v1.1
		// This means layout 1, no sample identifiers, zlib compression.
		genfile::bgen::Context outputContext = bgenView.context() ;
		outputContext.flags = genfile::bgen::e_Layout1 | genfile::bgen::e_ZlibCompression ;
		
		// Write offset and header
		genfile::bgen::write_offset( std::cout, outputContext.header_size() ) ;
		genfile::bgen::write_header_block( std::cout, outputContext ) ;

		// We require two buffers: one to serialise data to,
		// and one to assemble final compressed result to.
		std::vector< byte_t > serialisationBuffer( 6 * outputContext.number_of_samples ) ;
		std::vector< byte_t > idDataBuffer ;
		std::vector< byte_t > compressionBuffer ;

		std::string SNPID, rsid, chromosome ;
		uint32_t position ;
		std::vector< std::string > alleles ;

		std::vector< uint64_t > probability_encoding_table = compute_bgen_v11_probability_encoding_table() ;

		int const compressionLevel = options().get< int >( "-compression-level" ) ;

		{
			auto progress_context = ui().get_progress_context( "Processing " + std::to_string( bgenView.number_of_variants() ) + " variants" ) ;
			for( std::size_t i = 0; i < bgenView.number_of_variants(); ++i ) {
				bool success = bgenView.read_variant(
					&SNPID, &rsid, &chromosome, &position, &alleles
				) ;
				assert( success ) ;
				if( alleles.size() != 2 ) {
					std::cerr
						<< "In -transcode, found variant with " << alleles.size() << " allele, only 2 alleles are supported by BGEN v1.1.\n" ;
					throw std::invalid_argument( "bgen_filename=\"" + bgenView.file_metadata().filename + "\"" ) ;
				}
				
				genfile::bgen::write_snp_identifying_data(
					&idDataBuffer,
					outputContext,
					SNPID, rsid, chromosome, position,
					uint16_t( 2 ), 
					[&alleles](std::size_t i) { return alleles[i] ; }
				) ;

				genfile::bgen::v12::GenotypeDataBlock pack ;
				bgenView.read_and_unpack_v12_genotype_data_block( &pack ) ;

				if( pack.bits != 8 ) {
					std::cerr << "For -v11, expected 8 bits per probability, found " << pack.bits << ".\n" ;
					throw std::invalid_argument( "bgen_filename=\"" + bgenView.file_metadata().filename + "\"" ) ;
				}
				if( pack.phased != 0 ) {
					std::cerr << "For -v11, expected unphased data.\n" ;
					throw std::invalid_argument( "bgen_filename=\"" + bgenView.file_metadata().filename + "\"" ) ;
				}
				if( pack.end < pack.buffer + bgenView.context().number_of_samples ) {
					throw std::invalid_argument( "bgen_filename=\"" + bgenView.file_metadata().filename + "\"" ) ;
				}
				byte_t* out_p = &serialisationBuffer[0] ;
				byte_t const* p = pack.ploidy ;
				byte_t const* buffer = pack.buffer ;
				for( ; p < (pack.ploidy + pack.numberOfSamples); ++p, buffer += 2 ) {
					if( *p & byte_t( 0x80 ) ) {
						// data is missing, encode as zeros.
						*out_p++ = 0 ; *out_p++ = 0 ;
						*out_p++ = 0 ; *out_p++ = 0 ;
						*out_p++ = 0 ; *out_p++ = 0 ;
					} else {
						std::size_t const key = *reinterpret_cast< uint16_t const* >( buffer ) ;
						assert( key < probability_encoding_table.size() ) ;
						uint64_t const value = probability_encoding_table[ *reinterpret_cast< uint16_t const* >( buffer ) ] ;
#if DEBUG
						std::cerr << ( boost::format( "%d, %d" ) % key % probability_encoding_table.size() ) << "\n" ;
						std::cerr << ( boost::format( "%x: %x" ) % key % value ) << "\n" ;
						std::cerr << "Input: " << uint64_t(*reinterpret_cast< uint8_t const* >( buffer )) << ", " << uint64_t(*reinterpret_cast< uint8_t const* >( buffer+1 )) << "\n" ;
						std::cerr << "Output: " << uint64_t( value & 0xFFFF) << ", " << uint64_t( (value>>16) & 0xFFFF) << ", " << uint64_t( (value>>32) & 0xFFFF) << ".\n" ;
#endif
						*out_p++ = ( (value >> 0) & 0xFF ) ;
						*out_p++ = ( (value >> 8) & 0xFF ) ;
						*out_p++ = ( (value >> 16) & 0xFF ) ;
						*out_p++ = ( (value >> 24) & 0xFF ) ;
						*out_p++ = ( (value >> 32) & 0xFF ) ;
						*out_p++ = ( (value >> 40) & 0xFF ) ;
					}
				}
			
				// Compress it.
				assert( out_p == &serialisationBuffer[0] + serialisationBuffer.size() ) ;
				genfile::zlib_compress(
					serialisationBuffer,
					&compressionBuffer,
					compressionLevel
				) ;
#if DEBUG
				std::cerr << ( boost::format( "serialisation buffer: %d, id data Buffer: %d, result buffer: %d" ) % serialisationBuffer.size() % idDataBuffer.size() % compressionBuffer.size() ) << "\n" ;
#endif
				std::ostreambuf_iterator< char > outIt( std::cout ) ;
				std::copy( &idDataBuffer[0], &idDataBuffer[0]+idDataBuffer.size(), outIt ) ;
				genfile::bgen::write_little_endian_integer(
					std::cout,
					uint32_t( compressionBuffer.size() )
				) ;
				std::copy( &compressionBuffer[0], &compressionBuffer[0]+compressionBuffer.size(), outIt ) ;
				progress_context( i+1, bgenView.number_of_variants() ) ;
			}
		}
		
		std::cerr << boost::format( "# %s: success, total %d variants.\n" ) % globals::program_name % bgenView.number_of_variants() ;
	}
	
	std::vector< uint64_t > compute_bgen_v11_probability_encoding_table() const {
		// In bgen v1.2 8 bit encoding,
		// Each sample is encoded with 2 bytes.
		// First byte = prob1 * 255
		// Second byte = prob2 * 255
		// Third byte should make these add up to 255.
		std::vector< uint64_t > result( 65536, 0u ) ;
		for( uint16_t x = 0; x <= 255; ++x ) {
			for( uint16_t y = 0; y <= (255-x); ++y ) {
				uint16_t z = 255-x-y ;
				uint16_t key = (y<<8) | x ;
				// value is 32768.0
				uint64_t a = std::round((double(x)/255.0)*32768.0) ;
				uint64_t b = std::round((double(y)/255.0)*32768.0) ;
				uint64_t c = std::round((double(z)/255.0)*32768.0) ;
				uint64_t value = a | (b << 16) | (c << 32) ;
				result[key] = value ;
				//std::cerr << boost::format( "x=%d, y=%d, z=%d, a=%d, b=%d, c=%d" ) % x % y % z % a % b % c << ": " ;
				//std::cerr << boost::format( "result=%6x\n" ) % value ;
			}
		}
		return result ;
	}
	
	genfile::bgen::IndexQuery::GenomicRange parse_range( std::string const& spec ) const {
		std::size_t colon_pos = spec.find( ':' ) ;
		if ( colon_pos == std::string::npos ) {
			throw std::invalid_argument( "spec=\"" + spec + "\"" ) ;
		}

		std::vector< std::string > pieces ;
		pieces.push_back( spec.substr( 0, colon_pos )) ;
		pieces.push_back( spec.substr( colon_pos+1, spec.size() )) ;

		if( pieces.size() != 2 ) {
			throw std::invalid_argument( "spec=\"" + spec + "\"" ) ;
		}

		std::size_t separator_pos = pieces[1].find( '-' ) ;
		if ( separator_pos == std::string::npos ) {
			throw std::invalid_argument( "spec=\"" + spec + "\"" ) ;
		}

		std::string chromosome( pieces[0] ) ;
		int pos1 = (separator_pos == 0) ? 0 : std::stoi( pieces[1].substr( 0, separator_pos ) ) ;
		int pos2 = (separator_pos == (pieces[1].size()-1)) ? std::numeric_limits< int >::max() : std::stoi( pieces[1].substr( separator_pos + 1, pieces[1].size() ) ) ;
		assert( pos1 >= 0 ) ;
		assert( pos2 >= pos1 ) ;

		return genfile::bgen::IndexQuery::GenomicRange( chromosome, pos1, pos2 ) ;
	}	
} ;

int main( int argc, char** argv ) {
	std::ios_base::sync_with_stdio( false ) ;
	try {
		IndexBgenApplication app( argc, argv ) ;
	}
	catch( appcontext::HaltProgramWithReturnCode const& e ) {
		return e.return_code() ;
	}
	return 0 ;
}
