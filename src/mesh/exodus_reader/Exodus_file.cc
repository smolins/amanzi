#include <string>
#include <boost/format.hpp>
#include <exodusII.h>
#include "Exodus_file.hh"
#include "Exodus_error.hh"

namespace Amanzi {
namespace Exodus {

Exodus_file::Exodus_file (char const * filename_in) : system_word_size (sizeof (double)),
                                                      filename (filename_in), 
                                                      id (0), exodus_word_size (0), version (0.0)
{
    id = ex_open (filename, EX_READ, &system_word_size, &exodus_word_size, &version);
    if (id < 0) {
      std::string msg =
        boost::str(boost::format("%s: error: cannot open (%d)") %
                   filename_in % id);
      Exceptions::amanzi_throw( ExodusError (msg.c_str()) );
    }
}

void Exodus_file::to_stream (std::ostream& stream) const
{
    stream << "ExodusII file object:\n";
    stream << "  Path: " << filename << "\n";
    stream << "  File word size: " << exodus_word_size << "\n";
    stream << "  Version: " << version << "\n\n";
}

Exodus_file::~Exodus_file ()
{

}

} // namespace Exodus
} // namespace Amanzi

