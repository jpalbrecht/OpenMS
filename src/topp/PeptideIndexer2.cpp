// --------------------------------------------------------------------------
//                   OpenMS -- Open-Source Mass Spectrometry
// --------------------------------------------------------------------------
// Copyright The OpenMS Team -- Eberhard Karls University Tuebingen,
// ETH Zurich, and Freie Universitaet Berlin 2002-2016.
//
// This software is released under a three-clause BSD license:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of any author or any participating institution
//    may be used to endorse or promote products derived from this software
//    without specific prior written permission.
// For a full list of authors, refer to the file AUTHORS.
// --------------------------------------------------------------------------
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL ANY OF THE AUTHORS OR THE CONTRIBUTING
// INSTITUTIONS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
// OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
// OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
// ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// --------------------------------------------------------------------------
// $Maintainer: Jan Philipp Albrecht $
// $Authors: Jan Philipp Albrecht, Andreas Bertsch, Chris Bielow, Knut Reinert $
// --------------------------------------------------------------------------

#include <OpenMS/APPLICATIONS/TOPPBase.h>
#include <OpenMS/ANALYSIS/ID/PeptideIndexing2.h>
#include <OpenMS/CHEMISTRY/EnzymaticDigestion.h>
#include <OpenMS/CHEMISTRY/EnzymesDB.h>
#include <OpenMS/DATASTRUCTURES/ListUtils.h>
#include <OpenMS/FORMAT/IdXMLFile.h>
#include <OpenMS/FORMAT/FASTAFile.h>
#include <OpenMS/FORMAT/FileHandler.h>
#include <OpenMS/METADATA/ProteinIdentification.h>
#include <OpenMS/SYSTEM/File.h>

using namespace OpenMS;

//-------------------------------------------------------------
//Doxygen docu
//-------------------------------------------------------------

/**
    @page TOPP_PeptideIndexer PeptideIndexer

    @brief Refreshes the protein references for all peptide hits from an idXML file and adds target/decoy information.

<CENTER>
    <table>
        <tr>
            <td ALIGN = "center" BGCOLOR="#EBEBEB"> pot. predecessor tools </td>
            <td VALIGN="middle" ROWSPAN=2> \f$ \longrightarrow \f$ PeptideIndexer \f$ \longrightarrow \f$</td>
            <td ALIGN = "center" BGCOLOR="#EBEBEB"> pot. successor tools </td>
        </tr>
        <tr>
            <td VALIGN="middle" ALIGN = "center" ROWSPAN=1> @ref TOPP_IDFilter or @n any protein/peptide processing tool </td>
            <td VALIGN="middle" ALIGN = "center" ROWSPAN=1> @ref TOPP_FalseDiscoveryRate </td>
        </tr>
    </table>
</CENTER>

  A detailed description of the parameters and functionality is given in PeptideIndexing.

  All peptide and protein hits are annotated with target/decoy information, using the meta value "target_decoy". For proteins the possible values are "target" and "decoy", depending on whether the protein accession contains the decoy pattern (parameter @p decoy_string) as a suffix or prefix, respectively (see parameter @p prefix). For peptides, the possible values are "target", "decoy" and "target+decoy", depending on whether the peptide sequence is found only in target proteins, only in decoy proteins, or in both. The target/decoy information is crucial for the @ref TOPP_FalseDiscoveryRate tool. (For FDR calculations, "target+decoy" peptide hits count as target hits.)

  PeptideIndexer supports relative database filenames, which (when not found in the current working directory) are looked up in the directories specified
  by @p OpenMS.ini:id_db_dir (see @subpage TOPP_advanced).

  @note Currently mzIdentML (mzid) is not directly supported as an input/output format of this tool. Convert mzid files to/from idXML using @ref TOPP_IDFileConverter if necessary.

  @note The tool and its parameters are described in detail in the PeptideIndexing documentation. Please make sure you read and understand it before using the tool to avoid suboptimal results or long runtimes!

  <B>The command line parameters of this tool are:</B>
  @verbinclude TOPP_PeptideIndexer.cli
  <B>INI file documentation of this tool:</B>
  @htmlinclude TOPP_PeptideIndexer.html
*/


// We do not want this class to show up in the docu:
/// @cond TOPPCLASSES

class TOPPPeptideIndexer :
        public TOPPBase {
public:
    TOPPPeptideIndexer() :
            TOPPBase("PeptideIndexer",
                     "Refreshes the protein references for all peptide hits.") {
    }

protected:
    void registerOptionsAndFlags_() {
        registerInputFile_("in", "<file>", "", "Input idXML file containing the identifications.");
        setValidFormats_("in", ListUtils::create<String>("idXML"));

        // would love to take registerInputFile_ but this one trys to find the file under the given path.
        // but there is not ONE file to find because its an index... i dont know how to skip this

        registerOutputFile_("index", "<file>", "", "Input index file.", false);
        setValidFormats_("index", ListUtils::create<String>("txt"));

        registerOutputFile_("indexAAA", "<file>", "", "TEST",false);
        setValidFormats_("indexAAA", ListUtils::create<String>("txt"));

        registerOutputFile_("out", "<file>", "", "Output idXML file.");
        setValidFormats_("out", ListUtils::create<String>("idXML"));

        Param temp = PeptideIndexing2().getParameters();
        registerFullParam_(temp);
    }

    ExitCodes main_(int, const char **) {
        //-------------------------------------------------------------
        // parsing parameters
        //-------------------------------------------------------------
        String in = getStringOption_("in");
        String index = getStringOption_("index");
        String indexAAA = getStringOption_("indexAAA");
        String out = getStringOption_("out");

        PeptideIndexing2 indexer;
        Param param = getParam_().copy("", true);
        Param param_pi = indexer.getParameters();
        param_pi.update(param, false, Log_debug); // suppress param. update message
        indexer.setParameters(param_pi);

        //-------------------------------------------------------------
        // reading input
        //-------------------------------------------------------------

        // idXML
        std::vector<ProteinIdentification> prot_ids;
        std::vector<PeptideIdentification> pep_ids;
        IdXMLFile().load(in, prot_ids, pep_ids);


        //-------------------------------------------------------------
        // calculations
        //-------------------------------------------------------------

        PeptideIndexing2::ExitCodes indexer_exit = indexer.run(index,indexAAA,prot_ids,pep_ids);
        if ((indexer_exit != PeptideIndexing2::EXECUTION_OK) &&
            (indexer_exit != PeptideIndexing2::PEPTIDE_IDS_EMPTY)) {
            if (indexer_exit == PeptideIndexing2::DATABASE_EMPTY) {
                return INPUT_FILE_EMPTY;
            }
            else if (indexer_exit == PeptideIndexing2::UNEXPECTED_RESULT) {
                return UNEXPECTED_RESULT;
            }
            else if (indexer_exit == PeptideIndexing2::INPUT_ERROR) {
                return INPUT_FILE_CORRUPT;
            }
            else {
                return UNKNOWN_ERROR;
            }
        }

        //-------------------------------------------------------------
        // calculate protein coverage
        //-------------------------------------------------------------

        if (param.getValue("write_protein_sequence").toBool()) {
            for (Size i = 0; i < prot_ids.size(); ++i) {
                prot_ids[i].computeCoverage(pep_ids);
            }
        }

        //-------------------------------------------------------------
        // writing output
        //-------------------------------------------------------------
        IdXMLFile().store(out, prot_ids, pep_ids);

        return EXECUTION_OK;
    }

};

int main(int argc, const char **argv) {
    TOPPPeptideIndexer tool;
    return tool.main(argc, argv);

}

/// @endcond
