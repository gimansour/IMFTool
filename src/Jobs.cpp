/*  is a GUI application for interactive IMF Master Package creation.
 * Copyright(C) 2016 Björn Stresing
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.If not, see <http://www.gnu.org/licenses/>.
 */
#include "Jobs.h"
#include "AS_02.h"
#include "Metadata.h"
#include <vector>
#include "PCMParserList.h"
#include "AS_DCP_internal.h"
#include <QFileInfo>
#include <QCryptographicHash>
#include <QFile>
#include <QProcess>
#include <QDir>
//regxmllibc
#include <com/sandflow/smpte/regxml/dict/MetaDictionaryCollection.h>
#include <com/sandflow/smpte/regxml/dict/importers/XMLImporter.h>
#include "com/sandflow/smpte/regxml/MXFFragmentBuilder.h"
#include <fstream>

XERCES_CPP_NAMESPACE_USE

using namespace rxml;
//regxmllibc

JobCalculateHash::JobCalculateHash(const QString &rSourceFile) :
AbstractJob(tr("Calculating Hash: %1").arg(QFileInfo(rSourceFile).fileName())), mSourceFile(rSourceFile) {

}

Error JobCalculateHash::Execute() {

	QFile file(mSourceFile);
	if(file.open(QIODevice::ReadOnly) == false) {
		return Error(Error::SourceFileOpenError, file.fileName());
	}

	QCryptographicHash hasher(QCryptographicHash::Sha1);

	Error error;
	char buffer[16 * 1024];
	qint64 count;
	qint64 file_size = file.size();
	qint64 bytes_read = 0;
	int progress = 0;
	int last_progress = 0;
	do {
		if(QThread::currentThread()->isInterruptionRequested()) {
			error = Error(Error::WorkerInterruptionRequest);
			break;
		}
		count = file.read(buffer, sizeof(buffer));
		if(count == -1) {
			error = Error(Error::HashCalculation, tr("Couldn't read file for Hash calculation."));
			break;
		}
		bytes_read += count;
		progress = bytes_read * 100 / file_size;
		if(progress != last_progress) emit Progress(progress);
		last_progress = progress;
		hasher.addData(buffer, count);
	} while(!file.atEnd());

	if(error.IsError() == false) {
		emit Result(hasher.result(), GetIdentifier());
	}
	file.close();
	return error;
}

JobWrapWav::JobWrapWav(const QStringList &rSourceFiles, const QString &rOutputFile, const SoundfieldGroup &rSoundFieldGroup, const QUuid &rAssetId, const QString &rLanguageTag, const QString &rMCATitle, const QString &rMCATitleVersion, const QString &rMCAAudioContentKind, const QString &rMCAAudioElementKind) :
AbstractJob(tr("Wrapping %1").arg(QFileInfo(rOutputFile).fileName())), mOutputFile(rOutputFile), mSourceFiles(rSourceFiles), mSoundFieldGoup(rSoundFieldGroup), mLanguageTag(rLanguageTag), mMCATitle(rMCATitle), mMCATitleVersion(rMCATitleVersion), mMCAAudioContentKind(rMCAAudioContentKind), mMCAAudioElementKind(rMCAAudioElementKind), mWriterInfo() {

	convert_uuid(rAssetId, (unsigned char*)mWriterInfo.AssetUUID);
}

Error JobWrapWav::Execute() {

	Error error;
	if(mSourceFiles.isEmpty() == false) {
		if(mSoundFieldGoup.IsComplete() == true) {
			// An AS - 02 PCM file is clip - wrapped, but the interface defined below mimics that used
			// for frame-wrapped essence elsewhere in this library.  The concept of frame rate
			// therefore is only relevant to these classes and is not reflected in or affected by
			// the contents of the MXF file.
			ASDCP::PCM::FrameBuffer buffer;
			ASDCP::PCMParserList parser;
			ASDCP::PCM::AudioDescriptor audio_descriptor;
			AS_02::PCM::MXFWriter writer;
			const ASDCP::Dictionary *dict = &ASDCP::DefaultSMPTEDict();
			ASDCP::MXF::WaveAudioDescriptor *essence_descriptor = NULL;
			ASDCP::MXF::AS02_MCAConfigParser mca_config(dict);
			int progress = 0;
			int last_progress = 0;
			//ASDCP::Rational edit_rate;
			QFileInfo output_file(mOutputFile);

			if(output_file.exists() && output_file.isFile() && !output_file.isSymLink()) QFile::remove(output_file.absoluteFilePath());
			QFileInfo file_info(mSourceFiles.first());
			Kumu::PathList_t source_files;
			for(int i = 0; i < mSourceFiles.size(); i++) {
				source_files.push_back(mSourceFiles.at(i).toStdString());
			}
			// set up MXF writer
			Kumu::Result_t result = parser.OpenRead(source_files, ASDCP::Rational(24, 1)); // Dirty hack? We need the duration in multiples of samples.
			if(ASDCP_SUCCESS(result)) {
				result = parser.FillAudioDescriptor(audio_descriptor);
				audio_descriptor.EditRate = ASDCP::Rational(24, 1);
				buffer.Capacity(ASDCP::PCM::CalcFrameBufferSize(audio_descriptor));
				essence_descriptor = new ASDCP::MXF::WaveAudioDescriptor(dict);
				result = ASDCP::PCM_ADesc_to_MD(audio_descriptor, essence_descriptor);
				if (mLanguageTag.isEmpty()) {
					if(mca_config.DecodeString(mSoundFieldGoup.GetAsString().toStdString()) == false) {
						error = Error(Error::MCAStringDecoding);
						return error;
					}
				} else {
					if(mca_config.DecodeString(mSoundFieldGoup.GetAsString().toStdString(), mLanguageTag.toStdString()) == false) {
						error = Error(Error::MCAStringDecoding);
						return error;
					}
				}
				if(mca_config.ChannelCount() != essence_descriptor->ChannelCount) {
					error = Error(Error::ChannelCountMismatch);
					return error;
				}
				ASDCP::MXF::InterchangeObject_list_t::iterator i;
				for ( i = mca_config.begin(); i != mca_config.end(); ++i ) {
				  if ( (*i)->GetUL() == ASDCP::UL(dict->ul(MDD_SoundfieldGroupLabelSubDescriptor))) {
					  ASDCP::MXF::SoundfieldGroupLabelSubDescriptor *current_soundfield;
					  current_soundfield = reinterpret_cast<ASDCP::MXF::SoundfieldGroupLabelSubDescriptor*>(*i);
					  if (current_soundfield) {
						  // Set SoundfieldGroupLabelSubDescriptor items which are mandatory per ST 2067-2
						  if (!mMCATitle.isEmpty()) current_soundfield->MCATitle = mMCATitle.toStdString();
						  if (!mMCATitleVersion.isEmpty()) current_soundfield->MCATitleVersion = mMCATitleVersion.toStdString();
						  if (!mMCAAudioContentKind.isEmpty()) current_soundfield->MCAAudioContentKind = mMCAAudioContentKind.toStdString();
						  if (!mMCAAudioElementKind.isEmpty()) current_soundfield->MCAAudioElementKind = mMCAAudioElementKind.toStdString();
						  //current_soundfield->Dump();
					  }
				  }
				}
				essence_descriptor->ChannelAssignment = dict->ul(MDD_IMFAudioChannelCfg_MCA);
			}

			if(ASDCP_SUCCESS(result)) {
				result = writer.OpenWrite(output_file.absoluteFilePath().toStdString(), mWriterInfo, essence_descriptor, mca_config, audio_descriptor.EditRate);
			}

			if(ASDCP_SUCCESS(result)) {
				result = parser.Reset();
				unsigned int duration = 0;
				for(ui32_t frame_num = 0; ASDCP_SUCCESS(result); frame_num++) {
/*					if(frame_num >= audio_descriptor.ContainerDuration) {
						result = RESULT_ENDOFFILE; // We mustn't wrap the WAV footer.
						break;
					*/
					if(QThread::currentThread()->isInterruptionRequested()) {
						error = Error(Error::WorkerInterruptionRequest);
						writer.Finalize();
						QFile::remove(output_file.absoluteFilePath());
						return error;
					}
					result = parser.ReadFrame(buffer);
					if(ASDCP_SUCCESS(result)) {
						result = writer.WriteFrame(buffer);
						progress = frame_num * 100 / audio_descriptor.ContainerDuration;
						if(progress != last_progress) emit Progress(progress);
						last_progress = progress;
					}
				}
				if(result == RESULT_ENDOFFILE) result = RESULT_OK;
			}
			if(ASDCP_SUCCESS(result)) result = writer.Finalize();
			else {
				error = Error(result);
				writer.Finalize();
				QFile::remove(output_file.absoluteFilePath());
			}
		}
		else error = Error(Error::SoundfieldGroupIncomplete, mSoundFieldGoup.GetAsString());
	}
	else error = Error(Error::SourceFilesMissing);
	return error;
}




JobWrapTimedText::JobWrapTimedText(const QStringList &rSourceFiles, const QString &rOutputFile, const EditRate &rEditRate, const Duration &rDuration, const QUuid &rAssetId, const QString &rProfile, const QString &rLanguageTag) :
AbstractJob(tr("Wrapping %1").arg(QFileInfo(rOutputFile).fileName())), mOutputFile(rOutputFile), mSourceFiles(rSourceFiles), mEditRate(rEditRate), mDuration(rDuration), mProfile(rProfile), mLanguageTag(rLanguageTag) {

	convert_uuid(rAssetId, (unsigned char*)mWriterInfo.AssetUUID);

}


Error JobWrapTimedText::Execute() {

	Error error;

	AS_02::TimedText::ST2052_TextParser  Parser;
	AS_02::TimedText::MXFWriter Writer;
	TimedText::FrameBuffer buffer;
	AS_02::TimedText::TimedTextDescriptor TDesc;
	ASDCP::MXF::TimedTextDescriptor *essence_descriptor = NULL;

	QFileInfo output_file(mOutputFile);
	QFileInfo file_info(mSourceFiles.first());

	//We need to change the working directory to resolve the ancillary resources!
	QString dirCopyPath = QDir::currentPath();
	QDir::setCurrent(file_info.absolutePath());

	Result_t result = Parser.OpenRead(file_info.absoluteFilePath().toStdString(), mProfile.toStdString());
	result = Parser.FillTimedTextDescriptor(TDesc);

	TDesc.EditRate = ASDCP::Rational(mEditRate.GetNumerator(), mEditRate.GetDenominator());
	//WR
	TDesc.ContainerDuration = mDuration.GetCount();
	//WR
	TDesc.NamespaceName = mProfile.toStdString();
	Kumu::GenRandomUUID(TDesc.AssetID);
	buffer.Capacity(4*Kumu::Megabyte);

	ASDCP::MXF::InterchangeObject* tmp_obj = NULL;

	result = Writer.OpenWrite(output_file.absoluteFilePath().toStdString(), mWriterInfo, TDesc);

	if(ASDCP_SUCCESS(result)){
		//WR RFC5646LanguageTagList is available only in ASDCP::MXF::TimedTextDescriptor, not in AS_02::TimedText::TimedTextDescriptor
		result = Writer.OP1aHeader().GetMDObjectByType(DefaultCompositeDict().ul(MDD_TimedTextDescriptor), &tmp_obj);

		if(KM_SUCCESS(result)) {
			essence_descriptor = dynamic_cast<ASDCP::MXF::TimedTextDescriptor*>(tmp_obj);
			if(essence_descriptor == NULL) {
				error = Error(Error::UnsupportedEssence);
				return error;
			}
		}
		if (!mLanguageTag.isEmpty()) {
			essence_descriptor->RFC5646LanguageTagList.set(mLanguageTag.toStdString());
		}
		//WR
		std::string XMLDoc;
		AS_02::TimedText::ResourceList_t::const_iterator ri;

		result = Parser.ReadTimedTextResource(XMLDoc);
		if(ASDCP_SUCCESS(result)){
			result = Writer.WriteTimedTextResource(XMLDoc);

			if(ASDCP_SUCCESS(result)){

				for (ri = TDesc.ResourceList.begin(); ri != TDesc.ResourceList.end() && ASDCP_SUCCESS(result); ri++){

					result = Parser.ReadAncillaryResource((*ri).ResourceID, buffer);

					if(ASDCP_SUCCESS(result)){

						result = Writer.WriteAncillaryResource(buffer);
						if(ASDCP_FAILURE(result)) error = Error(result);
					}
					else { error = Error(result); QFile::remove(output_file.absoluteFilePath()); }
				}
			}
			else { error = Error(result); QFile::remove(output_file.absoluteFilePath()); }
		}
		else { error = Error(result); QFile::remove(output_file.absoluteFilePath()); }
	}
	else error = Error(result);

	result = Writer.Finalize();
	if(ASDCP_FAILURE(result)) { error = Error(result); QFile::remove(output_file.absoluteFilePath()); }

	//return to default working directory
	QDir::setCurrent(dirCopyPath);

	return error;
}
//WR

/*JobExtractEssenceDescriptor::JobExtractEssenceDescriptor(const QString &rSourceFile) :
AbstractJob(tr("Extracting Essence Descriptor from: %1").arg(QFileInfo(rSourceFile).fileName())), mSourceFile(rSourceFile) {

}

 Error JobExtractEssenceDescriptor::Execute() {

	QFile file(mSourceFile);
	if(file.open(QIODevice::ReadOnly) == false) {
		return Error(Error::SourceFileOpenError, file.fileName());
	}
	Error error;

	QString filePath = mSourceFile;
	QString qresult;
	QProcess *myProcess = new QProcess();
	const QString program = "java";
	QStringList arg;
	arg << "-cp";
	arg << QApplication::applicationDirPath() + QString("/regxmllib/regxmllib.jar");
	arg << "com.sandflow.smpte.tools.RegXMLDump";
	arg << "-ed";
	arg << "-d";
	arg << QApplication::applicationDirPath() + QString("/regxmllib/www-smpte-ra-org-reg-335-2012.xml");
	arg << QApplication::applicationDirPath() + QString("/regxmllib/www-smpte-ra-org-reg-335-2012-13-1-aaf.xml");
	arg << QApplication::applicationDirPath() + QString("/regxmllib/www-smpte-ra-org-reg-395-2014-13-1-aaf.xml");
	arg << QApplication::applicationDirPath() + QString("/regxmllib/www-smpte-ra-org-reg-2003-2012.xml");
	arg << "-i";
	arg << filePath;
	myProcess->start(program, arg);
	myProcess->waitForFinished(-1);
	if (myProcess->exitStatus() == QProcess::NormalExit) {
		if (myProcess->exitCode() != 0) { return error = Error(Error::ExitCodeNotZero); }
	} else {
		return error = Error(Error::ExitStatusError);
	}

	try {
		qresult = myProcess->readAllStandardOutput();
	}
	catch (...) {
		return error = Error(Error::EssenceDescriptorExtraction);
	}

	if(error.IsError() == false) {
		emit Result(qresult, GetIdentifier());
	}

	return error;
} */

JobExtractEssenceDescriptor::JobExtractEssenceDescriptor(const QString &rSourceFile) :
AbstractJob(tr("Extracting Essence Descriptor from: %1").arg(QFileInfo(rSourceFile).fileName())), mSourceFile(rSourceFile) {

}


Error JobExtractEssenceDescriptor::Execute() {

	QFile file(mSourceFile);
	if(file.open(QIODevice::ReadOnly) == false) {
		return Error(Error::SourceFileOpenError, file.fileName());
	}
	Error error;

	QList<QString> dicts_fname = QList<QString>()
/*		<< "www-smpte-ra-org-reg-335-2012-13-1-amwa-as12.xml"
		<< "www-smpte-ra-org-reg-335-2012-13-1-amwa-rules.xml"
		<< "www-smpte-ra-org-reg-335-2012-13-4-archive.xml"
		<< "www-smpte-ra-org-reg-335-2012-13-12-as11.xml"
		<< "www-smpte-ra-org-reg-335-2012-13-13.xml"
		<< "www-smpte-ra-org-reg-395-2014.xml"*/
		<< "www-smpte-ra-org-reg-395-2014-13-1-aaf.xml"
/*		<< "www-smpte-ra-org-reg-395-2014-13-1-amwa-as10.xml"
		<< "www-smpte-ra-org-reg-395-2014-13-1-amwa-as11.xml"
		<< "www-smpte-ra-org-reg-395-2014-13-1-amwa-as12.xml"
		<< "www-smpte-ra-org-reg-395-2014-13-1-amwa-as-common.xml"
		<< "www-smpte-ra-org-reg-395-2014-13-4-archive.xml"
		<< "www-smpte-ra-org-reg-395-2014-13-12-as11.xml"
		<< "www-smpte-ra-org-reg-395-2014-13-13.xml"*/
		<< "www-smpte-ra-org-reg-2003-2012.xml"
/*		<< "www-smpte-ra-org-reg-2003-2012-13-1-amwa-as11.xml"
		<< "www-smpte-ra-org-reg-2003-2012-13-1-amwa-as12.xml"
		<< "www-smpte-ra-org-reg-2003-2012-13-4-archive.xml"
		<< "www-smpte-ra-org-reg-2003-2012-13-12-as11.xml"
		<< "www-ebu-ch-metadata-schemas-ebucore-smpte-class13-element.xml"
		<< "www-ebu-ch-metadata-schemas-ebucore-smpte-class13-group.xml"
		<< "www-ebu-ch-metadata-schemas-ebucore-smpte-class13-type.xml" */
		<< "www-smpte-ra-org-reg-335-2012.xml"
		<< "www-smpte-ra-org-reg-335-2012-13-1-aaf.xml"
//		<< "www-smpte-ra-org-reg-335-2012-13-1-amwa-as10.xml"
//		<< "www-smpte-ra-org-reg-335-2012-13-1-amwa-as11.xml"
		;

	XMLPlatformUtils::Initialize();

	XercesDOMParser *parser = new XercesDOMParser();

	parser->setDoNamespaces(true);

	MetaDictionaryCollection mds;

	for (int i = 0; i < dicts_fname.size(); i++) {

		QString dict_path = QApplication::applicationDirPath() + QString("/regxmllib/") + dicts_fname[i];
		parser->parse(dict_path.toStdString().c_str());
		DOMDocument *doc = parser->getDocument();
		if (doc) {
			MetaDictionary *md = new MetaDictionary();
			XMLImporter::fromDOM(*doc, *md);
			mds.addDictionary(md);
		} else
		{
			qDebug() << "Meta Dictionary " << dict_path << " not found!";
			return Error(Error::MetaDictionaryOpenError, QString("Couldn't open file %1").arg(dict_path));
		}
	}

	XMLCh tempStr[3] = { chLatin_L, chLatin_S, chNull };
	DOMImplementation *impl = DOMImplementationRegistry::getDOMImplementation(tempStr);
	DOMLSOutput       *output = ((DOMImplementationLS*)impl)->createLSOutput();

	DOMDocument *doc = impl->createDocument();

	std::ifstream f(mSourceFile.toStdString().c_str(), std::ifstream::in | std::ifstream::binary);

	if (!f.good()) {
		qDebug() << "Can't read file:" << mSourceFile;
		error = Error(Error::EssenceDescriptorExtraction);

	}
	static const rxml::UL ESSENCE_DESCRIPTOR_KEY = "urn:smpte:ul:060e2b34.02010101.0d010101.01012400";
	const rxml::AUID* ed_auid = new rxml::AUID(ESSENCE_DESCRIPTOR_KEY);
	DOMDocumentFragment* frag = MXFFragmentBuilder::fromInputStream(f, mds, NULL, ed_auid, *doc);

	doc->appendChild(frag);


	if(error.IsError() == false) {
		emit Result(doc, GetIdentifier());
	}

	//doc->release();


	/* free heap */

	for (std::map<std::string, MetaDictionary*>::const_iterator it = mds.getDictionatries().begin();
		it != mds.getDictionatries().end();
		it++) {
		delete it->second;
	}
	XMLPlatformUtils::Terminate();
	return error;
}
//WR
