/**
 * Esri CityEngine SDK Maya Plugin Example
 *
 * This example demonstrates the main functionality of the Procedural Runtime API.
 * Esri R&D Center Zurich, Switzerland
 *
 * See http://github.com/ArcGIS/esri-cityengine-sdk for instructions.
 */

#include "node/PRTNode.h"
#include "node/Utilities.h"
#include "node/MayaCallbacks.h"

#include "prt/StringUtils.h"
#include "prt/FlexLicParams.h"

#include <maya/MFnPlugin.h>
#include <maya/MFnTransform.h>
#include <maya/MFnSet.h>
#include <maya/MFnPhongShader.h>
#include <maya/MDGModifier.h>
#include <maya/MItMeshPolygon.h>
#include <maya/MDagPath.h>
#include <maya/MItDependencyNodes.h>

#include <cstdio>
#include <sstream>

#ifdef _WIN32
#	include <Windows.h>
#else
#	include <dlfcn.h>
#endif


namespace {

const char*			FLEXNET_LIB			= "flexnet_prt";
const wchar_t*		PRT_EXT_SUBDIR		= L"ext";
const wchar_t*		ENC_MAYA			= L"MayaEncoder";
const MString		NAME_RULE_PKG		= "Rule_Package";
const prt::LogLevel	PRT_LOG_LEVEL		= prt::LOG_DEBUG;
const bool			ENABLE_LOG_CONSOLE	= true;
const bool			ENABLE_LOG_FILE		= false;

} // namespace


const wchar_t* ENC_ATTR      = L"com.esri.prt.core.AttributeEvalEncoder";
const char*    FILE_PREFIX   = "file:///";
const MString  NAME_GENERATE = "Generate_Model";


MTypeId PRTNode::theID(PRT_TYPE_ID);

PRTNode::PRTNode()
: mResolveMap(nullptr), mGenerateAttrs(nullptr), mMayaEncOpts(nullptr), mAttrEncOpts(nullptr)
, mEnums(nullptr), mHasMaterials(false), mCreatedInteractively(false)
{
	theNodeCount++;

	{
		prt::EncoderInfo  const* encInfo = prt::createEncoderInfo(ENC_MAYA);	
		encInfo->createValidatedOptionsAndStates(nullptr, &mMayaEncOpts, nullptr);
		encInfo->destroy();
	}

	{
		prt::EncoderInfo  const* encInfo = prt::createEncoderInfo(ENC_ATTR);	
		encInfo->createValidatedOptionsAndStates(nullptr, &mAttrEncOpts, nullptr);
		encInfo->destroy();
	}
}

PRTNode::~PRTNode() {
	if(mResolveMap)    {mResolveMap->destroy();    mResolveMap    = nullptr;}
	if(mGenerateAttrs) {mGenerateAttrs->destroy(); mGenerateAttrs = nullptr;}
	if(mMayaEncOpts)   {mMayaEncOpts->destroy();   mMayaEncOpts   = nullptr;}
	if(mAttrEncOpts)   {mAttrEncOpts->destroy();   mAttrEncOpts   = nullptr;}
	if(mEnums)         {destroyEnums();}

	if(--theNodeCount == 0) {
		theShadingGroups.clear();
	}

	prtu::dbg("PRTNode disposed\n");
}

MStatus PRTNode::setDependentsDirty(const MPlug& /*plugBeingDirtied*/, MPlugArray& affectedPlugs) {
	MPlug pOutMesh(thisMObject(), outMesh);
	affectedPlugs.append(pOutMesh);
	return MS::kSuccess;
}

MStatus PRTNode::preEvaluation(const MDGContext&, const MEvaluationNode&) {
	return MS::kSuccess;
}

MStatus PRTNode::compute(const MPlug& plug, MDataBlock& data ) {
	MStatus stat;
	mHasMaterials = false;

	MString dummy;
	std::string utf8Path(getStrParameter(rulePkg, dummy).asUTF8());
	std::vector<char> percentEncodedPath(2*utf8Path.size()+1);
	size_t len = percentEncodedPath.size();
	prt::StringUtils::percentEncode(utf8Path.c_str(), percentEncodedPath.data(), &len);
	if(len > percentEncodedPath.size()+1){
		percentEncodedPath.resize(len);
		prt::StringUtils::percentEncode(utf8Path.c_str(), percentEncodedPath.data(), &len);
	}

	std::string uri(FILE_PREFIX);
	uri.append(&percentEncodedPath[0]);

	if(mCreatedInteractively) {
		if(mLRulePkg.compare(uri)) {
			MGlobal::executeCommandOnIdle(MString("prtAttrs " + name()));
			return MS::kSuccess;
			mLRulePkg = uri;
		}
	} else {
		MFnDependencyNode fNode(thisMObject(), &stat);
		MCHECK(stat);
		PRTAttrs::updateRuleFiles(fNode, getStrParameter(rulePkg, dummy));
	}

	if(plug == outMesh && mGenerateAttrs) {
		bool connected = plug.isNetworked(&stat);
		MCHECK(stat);
		if(!connected)
			return MS::kFailure;

		MDataHandle inputHandle = data.inputValue(inMesh, &stat);
		MCHECK(stat);
		MObject iMesh = inputHandle.asMesh();

		updateShapeAttributes();

		if(getBoolParameter(mGenerate)) {
			MFnMesh iMeshFn(iMesh);

			MFloatPointArray vertices;
			MIntArray        pcounts;
			MIntArray        pconnect;

			iMeshFn.getPoints(vertices);
			iMeshFn.getVertices(pcounts, pconnect);

			double*   va = new double[vertices.length() * 3];
			uint32_t* ia = new uint32_t[pconnect.length()];
			uint32_t* ca = new uint32_t[pcounts.length()];

			for (int i = vertices.length(); --i >= 0;) {
				va[i * 3 + 0] = vertices[i].x;
				va[i * 3 + 1] = vertices[i].y;
				va[i * 3 + 2] = vertices[i].z;
			}
			pconnect.get((int*)ia);
			pcounts.get((int*)ca);

			MayaCallbacks* outputHandler = createOutputHandler(&plug, &data);

			prt::InitialShapeBuilder* isb = prt::InitialShapeBuilder::create();
			prt::Status setGeoStatus = isb->setGeometry(
					va,
					vertices.length()*3,
					ia,
					pconnect.length(),
					ca,
					pcounts.length()
			);
			if (setGeoStatus != prt::STATUS_OK)
				std::cerr << "InitialShapeBuilder setGeometry failed status = " << prt::getStatusDescription(setGeoStatus) << std::endl;

			isb->setAttributes(
					mRuleFile.c_str(),
					mStartRule.c_str(),
					prtu::computeSeed(vertices),
					L"",
					mGenerateAttrs,
					mResolveMap
			);

			const prt::InitialShape* shape          = isb->createInitialShapeAndReset();
			prt::Status              generateStatus = prt::generate(&shape, 1, 0, &ENC_MAYA, 1, &mMayaEncOpts, outputHandler, PRTNode::theCache, 0);
			if (generateStatus != prt::STATUS_OK)
				std::cerr << "prt generate failed: " << prt::getStatusDescription(generateStatus) << std::endl;
			isb->destroy();
			shape->destroy();

			delete[] ca;
			delete[] ia;
			delete[] va;
			delete   outputHandler;
		} else {
			MStatus stat;

			MDataHandle outputHandle = data.outputValue(plug, &stat);
			MCHECK(stat);

			MFnMeshData dataCreator;
			MObject newOutputData = dataCreator.create(&stat);
			MCHECK(stat);

			MFnMesh fnMesh;
			MObject oMesh = fnMesh.create(0, 0, MFloatPointArray(), MIntArray(), MIntArray(), newOutputData, &stat);
			MCHECK(stat);

			MCHECK(outputHandle.set(newOutputData));
		}

		data.setClean(plug);

		if(mCreatedInteractively) {
			MGlobal::executeCommand(mShadingCmd, DO_DBG, false);
			MGlobal::executeCommandOnIdle(MString("prtMaterials " + name()), DO_DBG);
		} else {
			MPlug pState(thisMObject(), state);
			int value;
			pState.getValue(value);
			if(value)
				pState.setValue(0);

			MPlug pHist(thisMObject(), isHistoricallyInteresting);
			pState.getValue(value);
			if(value != 2)
				pHist.setValue(2);
		}
	}

	mCreatedInteractively = true;
	return MS::kSuccess;
}


MStatus PRTNode::postEvaluation(const MDGContext&, const MEvaluationNode&, PostEvaluationType) {
	return MS::kSuccess;
}

void* PRTNode::creator() {
	return new PRTNode();
}

inline bool PRTNode::getBoolParameter(MObject & attr) {
	MPlug plug(thisMObject(), attr);
	if(attr.hasFn(MFn::kNumericAttribute)) {
		bool result;
		plug.getValue(result);
		return result;
	}
	return false;
}

inline MString & PRTNode::getStrParameter(MObject & attr, MString& value) {
	MPlug plug(thisMObject(), attr);

	if(attr.hasFn(MFn::kNumericAttribute)) {
		double fValue;
		plug.getValue(fValue);
		value.set(fValue);
	} else if(attr.hasFn(MFn::kTypedAttribute)) {
		plug.getValue(value);
	} else if(attr.hasFn(MFn::kEnumAttribute)) {
		short eValue;
		const PRTEnum* e = findEnum(attr);
		if(e) {
			plug.getValue(eValue);
			value = e->mSVals[eValue];
		}
	}
	return value;
}

MStatus PRTNode::updateShapeAttributes() {
	if(!(mGenerateAttrs)) return MS::kSuccess; 

	MStatus           stat;
	MObject           node = thisMObject();
	MFnDependencyNode fNode(node, &stat);
	MCHECK(stat);

	int count = (int)fNode.attributeCount(&stat);
	MCHECK(stat);

	prt::AttributeMapBuilder* aBuilder = prt::AttributeMapBuilder::create();

	for(int i = 0; i < count; i++) {
		MObject attr = fNode.attribute(i, &stat);
		if(stat != MS::kSuccess) continue;

		MPlug plug(node, attr);
		if(!(plug.isDynamic())) continue;

		MString       briefName = plug.partialName();
		std::wstring  name      = mBriefName2prtAttr[briefName.asWChar()];

		if(attr.hasFn(MFn::kNumericAttribute)) {
			MFnNumericAttribute nAttr(attr);

			if(nAttr.unitType() == MFnNumericData::kBoolean) {
				bool b, db; 
				nAttr.getDefault(db);
				MCHECK(plug.getValue(b));
				if(b != db)
					aBuilder->setBool(name.c_str(), b);
			} else if(nAttr.unitType() == MFnNumericData::kDouble) {
				double d, dd; 
				nAttr.getDefault(dd);
				MCHECK(plug.getValue(d));
				if(d != dd)
					aBuilder->setFloat(name.c_str(), d);
			} else if(nAttr.isUsedAsColor()) {
				float r;
				float g;
				float b;

				nAttr.getDefault(r, g, b);
				wchar_t dcolor[]  = L"#000000";
				prtu::toHex(dcolor, r, g, b);

				MObject rgb;
				MCHECK(plug.getValue(rgb));

				MFnNumericData fRGB(rgb);
				MCHECK(fRGB.getData(r, g, b));

				wchar_t color[]  = L"#000000";
				prtu::toHex(color, r, g, b);

				if(wcscmp(dcolor, color))
					aBuilder->setString(name.c_str(), color);
			}

		} else if(attr.hasFn(MFn::kTypedAttribute)) {
			MFnTypedAttribute tAttr(attr);
			MString       s;
			MFnStringData dsd;
			MObject       dso = dsd.create(&stat);
			MCHECK(stat);
			MCHECK(tAttr.getDefault(dso));

			MFnStringData fDs(dso, &stat);
			MCHECK(stat);

			MCHECK(plug.getValue(s));
			if(s != fDs.string(&stat)) {
				MCHECK(stat);
				aBuilder->setString(name.c_str(), s.asWChar());
			}
		} else if(attr.hasFn(MFn::kEnumAttribute)) {
			MFnEnumAttribute eAttr(attr);

			short di;
			short i;
			MCHECK(eAttr.getDefault(di));
			MCHECK(plug.getValue(i));
			if(i != di)
				aBuilder->setString(name.c_str(), eAttr.fieldName(i).asWChar());
		}
	}

	mGenerateAttrs->destroy();
	mGenerateAttrs = aBuilder->createAttributeMap();

	aBuilder->destroy();

	return MS::kSuccess;
}

void PRTNode::destroyEnums() {
	for(PRTEnum* e = mEnums; e;) {
		PRTEnum * tmp = e->mNext;
		delete e;
		e = tmp;
	}

	mEnums = nullptr;
}

const PRTEnum* PRTNode::findEnum(const MObject & attr) const {
	for(PRTEnum* e = mEnums; e; e = e->mNext) {
		if(e->mAttr.object() == attr)
			return e;
	}
	return nullptr;
}

MayaCallbacks* PRTNode::createOutputHandler(const MPlug* plug, MDataBlock* data) {
	return new MayaCallbacks(plug, data, &mShadingGroups, &mShadingRanges, &mShadingCmd);
}

void PRTNode::initLogger() {
	if (ENABLE_LOG_CONSOLE) {
		theLogHandler = prt::ConsoleLogHandler::create(prt::LogHandler::ALL, prt::LogHandler::ALL_COUNT);
		prt::addLogHandler(theLogHandler);
	}

	if (ENABLE_LOG_FILE) {
		std::string logPath = getPluginRoot() + prtu::getDirSeparator<char>() + "prt4maya.log";
		std::wstring wLogPath(logPath.length(), L' ');
		std::copy(logPath.begin(), logPath.end(), wLogPath.begin());
		theFileLogHandler = prt::FileLogHandler::create(prt::LogHandler::ALL, prt::LogHandler::ALL_COUNT, wLogPath.c_str());
		prt::addLogHandler(theFileLogHandler);
	}
}

// plugin root = location of prt4maya shared library
const std::string& PRTNode::getPluginRoot() {
	static std::string* rootPath = 0;
	if (rootPath == 0) {
#ifdef _MSC_VER
		char dllPath[_MAX_PATH];
		char drive[8];
		char dir[_MAX_PATH];
		HMODULE hModule = 0;

		GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)PRTNode::creator, &hModule);
		DWORD res = ::GetModuleFileName(hModule, dllPath, _MAX_PATH);
		if (res == 0) {
			// TODO DWORD e = ::GetLastError();
			throw std::runtime_error("failed to get plugin location");
		}

		_splitpath_s(dllPath, drive, 8, dir, _MAX_PATH, 0, 0, 0, 0);
		rootPath = new std::string(drive);
		rootPath->append(dir);
#else
		Dl_info dl_info;
		dladdr((void *)getPluginRoot, &dl_info);
		std::string tmp(dl_info.dli_fname);
		rootPath = new std::string(tmp.substr(0, tmp.find_last_of(prtu::getDirSeparator<char>()))); // accepted mem leak
#endif
		if (*rootPath->rbegin() != prtu::getDirSeparator<char>())
			rootPath->append(1, prtu::getDirSeparator<char>());
	}
	return *rootPath;
}

MStatus PRTNode::initialize() {
	MFnTypedAttribute   typedFn;
	MStatus             stat;

	outMesh = typedFn.create( "outMesh", "outMesh", MFnData::kMesh, &stat ); 
	MCHECK(stat);  
	typedFn.setStorable(false);
	typedFn.setWritable(false);
	stat = addAttribute( outMesh );
	MCHECK(stat);  

	inMesh = typedFn.create( "inMesh", "inMesh", MFnData::kMesh, &stat ); 
	MCHECK(stat);  
	typedFn.setStorable(false);
	typedFn.setHidden(true);
	MCHECK(addAttribute(inMesh));
	MCHECK(attributeAffects(inMesh, outMesh));

	MStatus           stat2;
	MFnStringData  	  stringData;
	MFnTypedAttribute fAttr;

	rulePkg = fAttr.create( NAME_RULE_PKG, "rulePkg", MFnData::kString, stringData.create(&stat2), &stat );
	MCHECK(stat2);
	MCHECK(stat);
	MCHECK(fAttr.setUsedAsFilename(true));
	MCHECK(fAttr.setCached    (true));
	MCHECK(fAttr.setStorable  (true));
	MCHECK(fAttr.setNiceNameOverride(MString("Rule Package(*.rpk)")));
	MCHECK(addAttribute(rulePkg));
	MCHECK(attributeAffects(rulePkg, outMesh ));

	return MS::kSuccess;
}

void PRTNode::uninitialize() {
	theCache->destroy();
	theLicHandle->destroy();

	if (ENABLE_LOG_CONSOLE) {
		prt::removeLogHandler(theLogHandler);
		theLogHandler->destroy();
	}
	if (ENABLE_LOG_FILE) {
		prt::removeLogHandler(theFileLogHandler);
		theFileLogHandler->destroy();
	}
}

namespace {

const char* ENV_LIC_FEATURE = "ESRI_CE_SDK_LIC_FEATURE";
const char* ENV_LIC_SERVER = "ESRI_CE_SDK_LIC_HOST";
const char* EMPTY_STRING = "";

bool tryToGetLicenseDetails(prt::FlexLicParams& flp) {
	const char* envLicFeature = getenv(ENV_LIC_FEATURE);
	const char* envLicServer = getenv(ENV_LIC_SERVER);
	if (envLicFeature == nullptr) {
		prt::log(L"prt4maya: could not get license feature type from environment", prt::LOG_FATAL);
		return false;
	}
	if ((strcmp(envLicFeature, "CityEngAdv") == 0) && (envLicServer == nullptr)) {
		prt::log(L"prt4maya: license type 'CityEngAdv' requires a license server hostname (<port>@<host>, e.g. 27000@flexnet.host.com)", prt::LOG_FATAL);
		return false;
	}
	flp.mFeature = envLicFeature;
	flp.mHostName = envLicServer != nullptr ? envLicServer : EMPTY_STRING;

	prtu::dbg("lic feature: '%s'", flp.mFeature);
	prtu::dbg("lic host: '%s'", flp.mHostName);

	return true;
}

} // namespace

P4M_API MStatus initializePlugin(MObject obj){
	PRTNode::initLogger();
	prtu::dbg("initialized prt logger");

	const std::string& pluginRoot   = PRTNode::getPluginRoot();
	std::wstring wPluginRoot(pluginRoot.length(), L' ');
	std::copy(pluginRoot.begin(), pluginRoot.end(), wPluginRoot.begin());

	const std::wstring prtExtPath = wPluginRoot + PRT_EXT_SUBDIR;
	prtu::wdbg(L"looking for prt extensions at %ls",prtExtPath.c_str());

	const std::string flexLibName = prtu::getSharedLibraryPrefix<char>() + std::string(FLEXNET_LIB) + prtu::getSharedLibrarySuffix<char>();
	prtu::dbg("flexLibName = %s", flexLibName.c_str());
	const std::string flexLibPath = pluginRoot + flexLibName;
	prtu::dbg("flexLibPath = %s", flexLibPath.c_str());

	prt::FlexLicParams flp;
	flp.mActLibPath = flexLibPath.c_str();
	if (!tryToGetLicenseDetails(flp))
		return MS::kFailure;	

	prt::Status status = prt::STATUS_UNSPECIFIED_ERROR;
	const wchar_t* prtExtPathPOD = prtExtPath.c_str();
	PRTNode::theLicHandle = prt::init(&prtExtPathPOD, 1, PRT_LOG_LEVEL, &flp, &status);

	if (PRTNode::theLicHandle == nullptr || status != prt::STATUS_OK)
		return MS::kFailure;

	PRTNode::theCache = prt::CacheObject::create(prt::CacheObject::CACHE_TYPE_DEFAULT);

	MFnPlugin plugin( obj, "Esri R&D Center Zurich", "1.0", "Any");

	MCHECK(plugin.registerNode("prt", PRTNode::theID, &PRTNode::creator, &PRTNode::initialize, MPxNode::kDependNode));
	MCHECK(plugin.registerUI("prt4mayaCreateUI", "prt4mayaDeleteUI"));
	MCHECK(plugin.registerCommand("prtAttrs",     PRTAttrs::creator));
	MCHECK(plugin.registerCommand("prtMaterials", PRTMaterials::creator));
	MCHECK(plugin.registerCommand("prtCreate",    PRTCreate::creator));

	return MS::kSuccess;
}

P4M_API MStatus uninitializePlugin( MObject obj) {
	PRTNode::uninitialize();

	MFnPlugin plugin( obj );

	MCHECK(plugin.deregisterCommand("prtCreate"));
	MCHECK(plugin.deregisterCommand("prtMaterials"));
	MCHECK(plugin.deregisterCommand("prtAttrs"));
	MCHECK(plugin.deregisterNode(PRTNode::theID));

	return MS::kSuccess;
}

// Main shape parameters
MObject PRTNode::rulePkg; 

// Input mesh
MObject PRTNode::inMesh;

// Output mesh
MObject PRTNode::outMesh;

// statics
prt::ConsoleLogHandler*	PRTNode::theLogHandler     = nullptr;
prt::FileLogHandler*	PRTNode::theFileLogHandler = nullptr;
const prt::Object*		PRTNode::theLicHandle      = nullptr;
prt::CacheObject*		PRTNode::theCache          = nullptr;
int						PRTNode::theNodeCount      = 0;
MStringArray			PRTNode::theShadingGroups;
