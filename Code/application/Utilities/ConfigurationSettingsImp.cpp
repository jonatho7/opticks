/*
 * The information in this file is
 * Copyright(c) 2007 Ball Aerospace & Technologies Corporation
 * and is subject to the terms and conditions of the
 * GNU Lesser General Public License Version 2.1
 * The license text is available from   
 * http://www.gnu.org/licenses/lgpl.html
 */

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QStringList>

#include "AppConfig.h"
#include "AppVerify.h"
#include "AppVersion.h"
#include "ArgumentList.h"
#include "BuildRevision.h"
#include "ConfigurationSettingsImp.h"
#include "DataDescriptor.h"
#include "DateTimeImp.h"
#include "DynamicObjectAdapter.h"
#include "Filename.h"
#include "ImportDescriptorImp.h"
#include "MessageLogMgrImp.h"
#include "ModelServices.h"
#include "MruFile.h"
#include "ObjectResource.h"
#include "PlugInDescriptor.h"
#include "PlugInManagerServicesImp.h"
#include "StringUtilities.h"
#include "UtilityServicesImp.h"
#include "xmlreader.h"
#include "xmlwriter.h"

#if defined(WIN_API)
#include <windows.h>
#include <stdlib.h>
#include <shlobj.h>
#include <shlwapi.h>
#else
#include <unistd.h>
#include <pwd.h>
#endif

using namespace std;

ConfigurationSettingsImp* ConfigurationSettingsImp::spInstance = NULL;
bool ConfigurationSettingsImp::mDestroyed = false;

ConfigurationSettingsImp* ConfigurationSettingsImp::instance()
{
   if (spInstance == NULL)
   {
      if(mDestroyed)
      {
         throw std::logic_error("Attempting to use ConfigurationSettings after "
            "destroying it.");
      }
      spInstance = new ConfigurationSettingsImp;
   }

   return spInstance;
}

void ConfigurationSettingsImp::destroy()
{
   if(mDestroyed)
   {
      throw std::logic_error("Attempting to destroy ConfigurationSettings after "
         "destroying it.");
   }
   delete spInstance;
   spInstance = NULL;
   mDestroyed = true;
}

ConfigurationSettingsImp::ConfigurationSettingsImp() : mIsInitialized(false)
{
   mCreator = APP_SPONSOR;
   mProduct = APP_NAME;
   mVersion = APP_VERSION_NUMBER;
   mpReleaseDate = NULL;
   mNeedToLoadMruFiles = true;
#if defined(_DEBUG)
   mProductionRelease = false;
#else
   mProductionRelease = APP_IS_PRODUCTION_RELEASE;
#endif
   mReleaseType = RT_NORMAL;

   mHomePath = locateApplicationHome();
   mUserDocs = locateUserDocs();

   //Perform a check to see if the app home env var is good, ie.
   //has SupportFiles available
   if (!QFile::exists(QString::fromStdString(mHomePath + "/SupportFiles/")))
   {
      mInitializationErrorMsg = string(APP_NAME) + " may be missing required support files."
                                " Please check your " + APP_NAME + " installation.";
   }

   UtilityServicesImp* pUtilities = NULL;
   pUtilities = UtilityServicesImp::instance();
   if (pUtilities != NULL)
   {
      mpReleaseDate = pUtilities->getDateTime();
      if (mpReleaseDate != NULL)
      {
         mpReleaseDate->set(APP_RELEASE_DATE_YEAR,
                            APP_RELEASE_DATE_MONTH,
                            APP_RELEASE_DATE_DAY);
      }
   }

   Service<ApplicationServices> pApp;
   pApp->attach(SIGNAL_NAME(ApplicationServices, ApplicationClosed), Slot(this, &ConfigurationSettingsImp::applicationClosed));

   // Load the values into config settings
   if (!deserialize())
   {
      mIsInitialized = false;
      mInitializationErrorMsg = "Unable to find application settings defaults, located in OPTICKS_HOME/DefaultSettings/1-ApplicationDefaults.cfg";
      return;
   }
   const ReleaseType* pReleaseType = dv_cast<ReleaseType>(&getSetting(ConfigurationSettingsImp::getSettingReleaseTypeKey()));
   if (pReleaseType != NULL)
   {
      mReleaseType = *pReleaseType;
   }

   // Set Internal Path -  Note that this only affects the environment
   // variable of the current process. 
   const Filename* pInternalPath = dv_cast<Filename>(&getSetting(ConfigurationSettings::getSettingInternalPathKey()));
   if (pInternalPath != NULL)
   {
      string internalPath = pInternalPath->getFullPathAndName();
      if (!internalPath.empty())
      {
         char *pPath = NULL;
         pPath = getenv("PATH");

         string path = "PATH=";
         if (pPath!=NULL) {
            path = path + pPath;
         }

   #if defined(WIN_API)
         internalPath = path+";"+internalPath;
         _putenv(const_cast<char*>(internalPath.c_str()));
   #else
         internalPath = path+":"+internalPath;
         putenv(const_cast<char*>(internalPath.c_str()));
   #endif
      }
   }

   // Reset Any Error Codes and mark as initialized
   mIsInitialized = true;
   mInitializationErrorMsg = "";
}

ConfigurationSettingsImp::~ConfigurationSettingsImp()
{
   notify(SIGNAL_NAME(Subject, Deleted));

   Service<ApplicationServices> pApp;
   pApp->detach(SIGNAL_NAME(ApplicationServices, ApplicationClosed), Slot(this, &ConfigurationSettingsImp::applicationClosed));

   // Delete the data descriptors in the MRU file vector
   vector<MruFile>::iterator iter;
   for (iter = mMruFiles.begin(); iter != mMruFiles.end(); ++iter)
   {
      MruFile mruFile = *iter;

      vector<ImportDescriptor*>::iterator iter2;
      for (iter2 = mruFile.mDescriptors.begin(); iter2 != mruFile.mDescriptors.end(); ++iter2)
      {
         ImportDescriptorImp* pImportDescriptor = dynamic_cast<ImportDescriptorImp*>(*iter2);
         if (pImportDescriptor != NULL)
         {
            delete pImportDescriptor;
         }
      }
   }

   UtilityServicesImp* pUtilities = NULL;
   pUtilities = UtilityServicesImp::instance();
   if (pUtilities != NULL)
   {
      if (mpReleaseDate != NULL)
      {
         pUtilities->destroyDateTime(mpReleaseDate);
      }
   }
}

string ConfigurationSettingsImp::locateApplicationHome()
{
   string homeLocation;

   const char* pHome = getenv("OPTICKS_HOME");
   if (pHome != NULL)
   {
      homeLocation = pHome;
   }

   if(!homeLocation.empty())
   {
      // OPTICKS_HOME was set as environment variable,
      // this should only be set for non-user installs, ie. developers
      // so go ahead and use that.
      QFileInfo homeDir(QString::fromStdString(homeLocation + "/"));

      if (homeDir.exists())
      {
         homeDir.makeAbsolute();
#if defined(WIN_API)
         homeLocation = homeDir.canonicalPath().toStdString();
#else
         homeLocation = homeDir.absolutePath().toStdString();
#endif
      }
   }
   
   if(homeLocation.empty())
   {
      //attempt to auto-discover OPTICKS_HOME, by looking one directory up
      //from the application directory
      QString moveUp = QString::fromStdString("/../");
      QString newHome = QCoreApplication::applicationDirPath().append(moveUp);
      QFileInfo homeDir(newHome);
      homeDir.makeAbsolute();
#if defined(WIN_API)
      homeLocation = homeDir.canonicalPath().toStdString();
#else
      homeLocation = homeDir.absolutePath().toStdString();
#endif
   }
   return homeLocation;
}

string ConfigurationSettingsImp::getHome() const
{
   return mHomePath;
}

string ConfigurationSettingsImp::getUserDocs() const
{
   return mUserDocs;
}

string ConfigurationSettingsImp::getCreator() const
{
   return mCreator;
}

string ConfigurationSettingsImp::getProduct() const
{
   return mProduct;
}

string ConfigurationSettingsImp::getVersion() const
{
   return mVersion;
}

string ConfigurationSettingsImp::getBuildRevision() const
{
   return string(BUILD_REVISION);
}

string ConfigurationSettingsImp::getUserName() const
{
   const int BUFFER_SIZE = 8096;
   string username;
#if defined(WIN_API)
   char* pUserName = new char[BUFFER_SIZE];
   DWORD size = BUFFER_SIZE;

   if (GetUserName(pUserName, &size) != false)
   {
      username = pUserName;
   }

   delete pUserName;
#else
   struct passwd* userEntry = getpwuid(getuid());
   if (userEntry != NULL)
   {
      if (userEntry->pw_name != NULL)
      {
         username = userEntry->pw_name;
      }
   }
#endif
   return username;
}

string ConfigurationSettingsImp::getOperatingSystemName() const
{
   std::string os;
#if defined(WIN_API)
   os = "Windows";
#elif defined(SOLARIS)
   os = "Solaris";
#else
   os = "Unkown"
#endif
   return os;
}

string ConfigurationSettingsImp::getArchitectureName() const
{
   string arch = "Unknown";
#if defined(WIN_API)
#if PTR_SIZE == 8
   arch = "x86-64";
#elif PTR_SIZE == 4
   arch = "x86-32";
#endif
#elif defined(SOLARIS)
#if PTR_SIZE == 8
   arch = "sparcv9";
#endif
#endif
   return arch;
}

const DateTime* ConfigurationSettingsImp::getReleaseDate() const
{
   return mpReleaseDate;
}

bool ConfigurationSettingsImp::isProductionRelease() const
{
   return mProductionRelease;
}

ReleaseType ConfigurationSettingsImp::getReleaseType() const
{
   return mReleaseType;
}

bool ConfigurationSettingsImp::isInitialized()
{
   return mIsInitialized;
}

const char* ConfigurationSettingsImp::getInitializationErrorMsg()
{
   if (mInitializationErrorMsg.empty())
   {
      return NULL;
   }

   const char* pErrorMsg = NULL;
   pErrorMsg = mInitializationErrorMsg.c_str();

   return pErrorMsg;
}

bool ConfigurationSettingsImp::setSetting(const std::string &key, const DataVariant &var, bool setIfSame)
{
   bool same = false;

   const string strKey = translateKey(key);

   if (!setIfSame)
   {
      DataVariant curValue = getSetting(strKey);
      try
      {
         //Need try-catch because DataVariant comparison can
         //throw UnsupportedOperation if the held type doesn't support comparision
         same = (var == curValue);
      }
      catch (DataVariant::UnsupportedOperation) {}
   }
   bool success = true;
   if (!same)
   {
      success = mpUserSettings->setAttributeByPath(strKey, var);
      if (success)
      {
         deleteSessionSetting(strKey);
         notify(SIGNAL_NAME(ConfigurationSettings, SettingModified), boost::any(strKey));
      }
   }
   return success;
}

const DataVariant &ConfigurationSettingsImp::getSetting(const string& key) const
{
   static DataVariant empty;

   const string strKey = translateKey(key);

   const DataVariant &sessionValue = mpSessionSettings->getAttributeByPath(strKey);
   if (sessionValue.isValid())
   {
      return sessionValue;
   }
   const DataVariant &userValue = mpUserSettings->getAttributeByPath(strKey);
   if (userValue.isValid())
   {
      return userValue;
   }
   const DataVariant &defaultValue = mpDefaultSettings->getAttributeByPath(strKey);
   if (defaultValue.isValid())
   {
      return defaultValue;
   }


   return empty;
}

bool ConfigurationSettingsImp::isUserSetting(const string& key) const
{
   const string strKey = translateKey(key);

   const DataVariant &sessionValue = mpSessionSettings->getAttributeByPath(strKey);
   if (sessionValue.isValid())
   {
      return false;
   }
   const DataVariant &userValue = mpUserSettings->getAttributeByPath(strKey);
   return userValue.isValid();
}

void ConfigurationSettingsImp::deleteUserSetting(const string& key)
{
   const string strKey = translateKey(key);

   bool success = mpUserSettings->removeAttributeByPath(strKey);
   if (success)
   {
      notify(SIGNAL_NAME(Subject, Modified));
   }
}

void ConfigurationSettingsImp::deleteSessionSetting(const std::string& key)
{
   const string strKey = translateKey(key);

   bool success = mpSessionSettings->removeAttributeByPath(strKey);
   if (success)
   {
      notify(SIGNAL_NAME(Subject, Modified));
   }
}

bool ConfigurationSettingsImp::setSessionSetting(const string& key, const DataVariant& var)
{
   const string strKey = translateKey(key);

   bool success = mpSessionSettings->setAttributeByPath(strKey, var);
   if (success)
   {
      notify(SIGNAL_NAME(ConfigurationSettings, SettingModified), boost::any(strKey));
   }
   return success;
}

void ConfigurationSettingsImp::copySetting(const string& key, DynamicObject* pObject) const
{
   const string strKey = translateKey(key);

   const DataVariant& dv = getSetting(strKey);
   if (dv.isValid())
   {
      pObject->setAttributeByPath(strKey, dv);
   }
}

string ConfigurationSettingsImp::translateKey(const string& key) const
{
   string strKey;
   if (key == "FileLocations/ImportExportPath")
   {
      strKey = getSettingImportPathKey();
   }
   else
   {
      strKey = key;
   }

   return strKey;
}

void ConfigurationSettingsImp::setMruFiles(const vector<MruFile>& mruFiles)
{
   mMruFiles = mruFiles;
}

void ConfigurationSettingsImp::removeMruFile(const string& filename)
{
   if (filename.empty() == true)
   {
      return;
   }

   // Make sure the MRU files have been deserialized before trying to remove the file
   deserializeMruFiles();

   Service<ModelServices> pModel;

   QString strFilename = QString::fromStdString(filename).toLower();
   strFilename.replace(QRegExp("\\\\"), "/");

   for (vector<MruFile>::iterator iter = mMruFiles.begin(); iter != mMruFiles.end(); ++iter)
   {
      MruFile mruFile = *iter;

      QString strMruFilename = QString::fromStdString(mruFile.mName).toLower();
      strMruFilename.replace(QRegExp("\\\\"), "/");

      if (strMruFilename == strFilename)
      {
         // Destroy the import descriptors in this MRU file
         vector<ImportDescriptor*>::iterator descriptorIter;
         for (descriptorIter = mruFile.mDescriptors.begin();
              descriptorIter != mruFile.mDescriptors.end();
              ++descriptorIter)
         {
            ImportDescriptor* pImportDescriptor = *descriptorIter;
            if (pImportDescriptor != NULL)
            {
               pModel->destroyImportDescriptor(pImportDescriptor);
            }
         }

         mMruFiles.erase(iter);
         break;
      }
   }
}

const vector<MruFile>& ConfigurationSettingsImp::getMruFiles() const
{
   const_cast<ConfigurationSettingsImp*>(this)->deserializeMruFiles();
   return mMruFiles;
}

void ConfigurationSettingsImp::deserializeMruFiles()
{
   if (!mNeedToLoadMruFiles)
   {
      return;
   }

   FactoryResource<Filename> pFilename;
   pFilename->setFullPathAndName(getUserSettingsFile());
   XmlReader xmlReader(NULL, false);
   XERCES_CPP_NAMESPACE_QUALIFIER DOMDocument *pDomDoc(NULL);
   pDomDoc = xmlReader.parse(pFilename.get());

   if (pDomDoc == NULL)
   {
      mNeedToLoadMruFiles = false;
      return;
   }

   XERCES_CPP_NAMESPACE_QUALIFIER DOMNodeList *pConfList(NULL);
   pConfList = pDomDoc->getElementsByTagName(X("ConfigurationSettings"));
   if (pConfList == NULL || pConfList->getLength() != 1)
   {
      mNeedToLoadMruFiles = false;
      return;
   }

   XERCES_CPP_NAMESPACE_QUALIFIER DOMNodeList *pSettingsNodes(NULL);
   pSettingsNodes = (pConfList->item(0))->getChildNodes();
   for (unsigned int i = 0; i < pSettingsNodes->getLength(); i++)
   {
      string name;
      XERCES_CPP_NAMESPACE_QUALIFIER DOMNode *pSettingsNode = pSettingsNodes->item(i);
      int nodeType = pSettingsNode->getNodeType();
      string elementName = A(pSettingsNode->getNodeName());
      if ((nodeType == XERCES_CPP_NAMESPACE_QUALIFIER DOMNode::ELEMENT_NODE) &&
          (elementName == "group") && (pSettingsNode->hasAttributes()))
      {
         XERCES_CPP_NAMESPACE_QUALIFIER DOMNamedNodeMap *pAttr = pSettingsNode->getAttributes();
         XERCES_CPP_NAMESPACE_QUALIFIER DOMNode *pNameAttr = pAttr->getNamedItem(X("name"));
         if (pNameAttr != NULL)
         {
            name = A(pNameAttr->getNodeValue());
         }
      }
      else
      {
         continue;
      }
      if (name == "MRUFiles")
      {
         Service<ModelServices> pModel;
         for(XERCES_CPP_NAMESPACE_QUALIFIER DOMNode *pMruFileNode = 
                                    pSettingsNode->getFirstChild();
                      pMruFileNode != NULL;
                      pMruFileNode = pMruFileNode->getNextSibling())
         {
            if(XERCES_CPP_NAMESPACE_QUALIFIER XMLString::equals(pMruFileNode->getNodeName(),
                                                                X("attribute")))
            {
               vector<ImportDescriptor*> descriptors;
               if(!static_cast<XERCES_CPP_NAMESPACE_QUALIFIER DOMElement*>(
                        pMruFileNode)->hasAttribute(X("name")))
               {
                  continue;
               }
               for(XERCES_CPP_NAMESPACE_QUALIFIER DOMNode *pDescriptorNode = 
                                             pMruFileNode->getFirstChild();
                           pDescriptorNode != NULL;
                           pDescriptorNode = pDescriptorNode->getNextSibling())
               {
                  if(XERCES_CPP_NAMESPACE_QUALIFIER XMLString::equals(pDescriptorNode->getNodeName(),
                                                                X("DataDescriptor")))
                  {
                     XERCES_CPP_NAMESPACE_QUALIFIER DOMElement *pElement =
                           static_cast<XERCES_CPP_NAMESPACE_QUALIFIER DOMElement*>(pDescriptorNode);
                     if(! (pElement->hasAttribute(X("type")) &&
                           pElement->hasAttribute(X("version"))))
                     {
                        continue;
                     }
                     string mruName = A(pElement->getAttribute(X("name")));
                     string type = A(pElement->getAttribute(X("type")));
                     unsigned int version = atoi(A(pElement->getAttribute(X("version"))));
                     DataDescriptor *pDescriptor = pModel->createDataDescriptor(mruName, type, NULL);
                     if(pDescriptor == NULL)
                     {
                        continue;
                     }
                     try
                     {
                        if(pDescriptor->fromXml(pElement, version))
                        {
                           ImportDescriptor* pImportDescriptor = pModel->createImportDescriptor(pDescriptor);
                           if (pImportDescriptor != NULL)
                           {
                              descriptors.push_back(pImportDescriptor);
                           }
                        }
                     }
                     catch(XmlBase::XmlException &)
                     {
                        // do nothing
                     }
                  }
               }
               string mruName = A(static_cast<XERCES_CPP_NAMESPACE_QUALIFIER DOMElement*>(
                                                pMruFileNode)->getAttribute(X("name")));
               string mruImporter = A(static_cast<XERCES_CPP_NAMESPACE_QUALIFIER DOMElement*>(
                                                pMruFileNode)->getAttribute(X("importer")));
               string mruModification = A(static_cast<XERCES_CPP_NAMESPACE_QUALIFIER DOMElement*>(
                                                pMruFileNode)->getAttribute(X("modification_time")));
               if ((mruName.empty() == false) && (mruImporter.empty() == false) && (mruModification.empty() == false))
               {
                  MruFile mruFile(mruName, mruImporter, descriptors, DateTimeImp(mruModification));
                  mMruFiles.push_back(mruFile);
               }
            }
         }
      }
   }

   mNeedToLoadMruFiles = false;
}

void ConfigurationSettingsImp::applicationClosed(Subject& subject, const std::string& signal, const boost::any &args)
{
   serialize();
}

string ConfigurationSettingsImp::getUserSettingsFile(bool createDir) const
{
   string configDir = ArgumentList::instance()->getOption("configDir");

   QDir configDirectory;
   if (configDir.empty())
   {
      QString userSettingsPath;
      QString appNamePath;
   #if defined(WIN_API)
      char path[MAX_PATH];
      HRESULT retValue = SHGetFolderPath(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, path);
      if(SUCCEEDED(retValue))
      {
         userSettingsPath = QString::fromAscii(path);
      }
      appNamePath = "Opticks";
   #else
      char* pPath = getenv("HOME");
      if (pPath != NULL)
      {
         userSettingsPath = QString::fromAscii(pPath);
      }
      appNamePath = ".opticks";
   #endif
      if (!userSettingsPath.isEmpty())
      {
         userSettingsPath.replace("\\", "/");
         QDir userSettingsDir(userSettingsPath);
         if (!userSettingsDir.exists())
         {
            return "";
         }
         QString appUserSettingsPath = userSettingsDir.absoluteFilePath(appNamePath);
         QDir appSettingsDir(appUserSettingsPath);
         if (!appSettingsDir.exists() && createDir)
         {
            userSettingsDir.mkdir(appNamePath);
         }
         configDirectory = appSettingsDir;
      }
      else
      {
         return "";
      }
   }
   else
   {
      QDir userConfigDir = QDir(QString::fromStdString(configDir));
      if (!userConfigDir.exists())
      {
         return "";
      }
      configDirectory = userConfigDir;
   }

   string os = getOperatingSystemName();
   string arch = getArchitectureName();
   string mode = "";
   #if defined(DEBUG)
      mode = "Debug";
   #endif

   QString cfgFilePath = configDirectory.absoluteFilePath(QString::fromStdString("UserSettings-" + mVersion + "-" + os + "-" + arch + mode + ".cfg"));
   return QDir::toNativeSeparators(cfgFilePath).toStdString();
}

string ConfigurationSettingsImp::locateUserDocs()
{
   string userDirectory;
   QString userFolderPath;
   QString appNamePath;
#if defined(WIN_API)
   char path[MAX_PATH];
   HRESULT retValue = SHGetFolderPath(NULL, CSIDL_PERSONAL | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, path);
   if(SUCCEEDED(retValue))
   {
      userFolderPath = QString::fromAscii(path);
   }
#else
   char* pPath = getenv("HOME");
   if (pPath != NULL)
   {
      userFolderPath = QString::fromAscii(pPath);
   }
#endif
   appNamePath = APP_NAME;
   if (!userFolderPath.isEmpty())
   {
      userFolderPath.replace("\\", "/");
      QDir userFolderDir(userFolderPath);
      if (!userFolderDir.exists())
      {
         return "";
      }
      QString appUserPath = userFolderDir.absoluteFilePath(appNamePath);
      QDir appUserDir(appUserPath);
      if (!appUserDir.exists())
      {
         userFolderDir.mkdir(appNamePath);
      }
      userDirectory = appUserDir.absolutePath().toStdString();
   }

   return userDirectory;
}

bool ConfigurationSettingsImp::serialize() const
{
   return serializeSettings(getUserSettingsFile(true), mpUserSettings.get(), true);
}

bool ConfigurationSettingsImp::serializeAsDefaults(const Filename* pFilename, const DynamicObject* pObject) const
{
   if (pFilename == NULL)
   {
      return false;
   }
   return serializeSettings(pFilename->getFullPathAndName(), pObject, false);
}

bool ConfigurationSettingsImp::deserialize()
{
   string defaultDir = mHomePath + SLASH + "DefaultSettings";
   string customDefaultDir = ArgumentList::instance()->getOption("defaultDir");

   QStringList defaultDirectories;
   defaultDirectories.append(QString::fromStdString(defaultDir));
   if (!customDefaultDir.empty())
   {
      defaultDirectories.append(QString::fromStdString(customDefaultDir));
   }

   //locate all .cfg files found in all default directories and sort them into
   //a map based on load order
   multimap<unsigned int, string> configFileMap;
   QStringList::iterator dirIter;
   QStringList cfgExtensions;
   cfgExtensions.append("*.cfg");
   for (dirIter = defaultDirectories.begin(); dirIter != defaultDirectories.end(); ++dirIter)
   {
      QDir curDir(*dirIter);
      if (curDir.exists())
      {
         QFileInfoList cfgFiles = curDir.entryInfoList(cfgExtensions, QDir::Files, QDir::Name);
         QFileInfoList::iterator cfgIter;
         for (cfgIter = cfgFiles.begin(); cfgIter != cfgFiles.end(); ++cfgIter)
         {
            QFileInfo cfgFile = *cfgIter;
            QStringList entries = cfgFile.fileName().split("-");
            if (entries.size() >= 2) //Require .cfg files to be named like "1-foobar.cfg"
            {
               QString loadOrderStr = entries[0];
               bool success;
               unsigned int loadOrder = loadOrderStr.toUInt(&success);
               if (success)
               {
                  configFileMap.insert(pair<const unsigned int, string>(loadOrder, cfgFile.absoluteFilePath().toStdString()));
               }
            }
         }
      }
   }

   //we must at least have "1-ApplicationDefaults.cfg" which contains the application default settings
   if (configFileMap.count(1) != 1)
   {
      return false;
   }
   QFileInfo appDefaultConfigFileName(QString::fromStdString(configFileMap.find(1)->second));
   if (appDefaultConfigFileName.fileName() != "1-ApplicationDefaults.cfg")
   {
      return false;
   }

   mpUserSettings->clear();
   mpSessionSettings->clear();
   mpDefaultSettings->clear();
   mNeedToLoadMruFiles = true;

   //parse all .cfg's files into mpDefaultSettings using the
   //load order
   multimap<unsigned int, string>::iterator configFileIter;
   for (configFileIter = configFileMap.begin(); configFileIter != configFileMap.end(); ++configFileIter)
   {
      FactoryResource<Filename> pFilename;
      pFilename->setFullPathAndName(configFileIter->second);
      FactoryResource<DynamicObject> pObj(deserialize(pFilename.get()));
      mpDefaultSettings->merge(pObj.get());
   }

   //parse the user's config file
   FactoryResource<Filename> pFilename;
   pFilename->setFullPathAndName(getUserSettingsFile());
   FactoryResource<DynamicObject> pObj(deserialize(pFilename.get()));
   if (pObj.get() != NULL)
   {
      mpUserSettings->merge(pObj.get());
   }

   notify(SIGNAL_NAME(Subject, Modified));

   return true;
}

DynamicObject* ConfigurationSettingsImp::deserialize(const Filename* pFilename) const
{
   if (pFilename == NULL)
   {
      return NULL;
   }
   FactoryResource<DynamicObject> pObj;
   XmlReader xmlReader(NULL, false);
   XERCES_CPP_NAMESPACE_QUALIFIER DOMDocument *pDomDoc(NULL);
   pDomDoc = xmlReader.parse(pFilename);

   if (pDomDoc == NULL)
   {
      return NULL;
   }

   XERCES_CPP_NAMESPACE_QUALIFIER DOMNodeList *pConfList(NULL);
   pConfList = pDomDoc->getElementsByTagName(X("ConfigurationSettings"));
   if (pConfList == NULL || pConfList->getLength() != 1)
   {
      return NULL;
   }

   XERCES_CPP_NAMESPACE_QUALIFIER DOMNodeList *pSettingsNodes(NULL);
   pSettingsNodes = (pConfList->item(0))->getChildNodes();
   for (unsigned int i = 0; i < pSettingsNodes->getLength(); i++)
   {
      string name;
      XERCES_CPP_NAMESPACE_QUALIFIER DOMNode *pSettingsNode = pSettingsNodes->item(i);
      int nodeType = pSettingsNode->getNodeType();
      string elementName = A(pSettingsNode->getNodeName());
      if ((nodeType == XERCES_CPP_NAMESPACE_QUALIFIER DOMNode::ELEMENT_NODE) &&
          (elementName == "group") && (pSettingsNode->hasAttributes()))
      {
         XERCES_CPP_NAMESPACE_QUALIFIER DOMNamedNodeMap *pAttr = pSettingsNode->getAttributes();
         XERCES_CPP_NAMESPACE_QUALIFIER DOMNode *pNameAttr = pAttr->getNamedItem(X("name"));
         if (pNameAttr != NULL)
         {
            name = A(pNameAttr->getNodeValue());
         }
      }
      else
      {
         continue;
      }
      if (name == "settings")
      {
         pObj->fromXml(pSettingsNode, XmlBase::VERSION); 
      }
   }

   return pObj.release();
}

void ConfigurationSettingsImp::updateProductionStatus()
{
   if(mReleaseType != RT_NORMAL && mReleaseType != RT_PROTO)
   {
      mProductionRelease = false;
      return;
   }

#if defined(_DEBUG)
   mProductionRelease = false;
#else
   mProductionRelease = APP_IS_PRODUCTION_RELEASE;
#endif

#pragma message(__FILE__ "(" STRING(__LINE__) ") : warning : Re-enable checking of plug-in production status to determine application production status. (kstreith)")
   /*
   if(mProductionRelease)
   {
      // search all plugins until we get a false
      Service<PlugInManagerServices> pPlugInManager;
      vector<PlugInDescriptor*> plugins = pPlugInManager->getPlugInDescriptors();
      for(vector<PlugInDescriptor*>::const_iterator plugin = plugins.begin();
          mProductionRelease && (plugin != plugins.end());
          ++plugin)
      {
         PlugInDescriptor *pDesc = *plugin;
         if (pDesc == NULL)
         {
            continue;
         }
         mProductionRelease = pDesc->isProduction();
      }
   }
   */
}

bool ConfigurationSettingsImp::serializeSettings(const string& filename, const DynamicObject* pSettings,
                                                 bool saveMru) const
{
   if (filename.empty())
   {
      return false;
   }

   const_cast<ConfigurationSettingsImp*>(this)->notify(SIGNAL_NAME(ConfigurationSettings, AboutToSave));

   XMLWriter xmlWriter("ConfigurationSettings");

   // Version, release date, and build date
   string dateFormat = "%d %B %Y";

   string releaseText = mpReleaseDate->getFormattedUtc(dateFormat);
   string buildRevision = getBuildRevision();

   xmlWriter.pushAddPoint(xmlWriter.addElement("opticks"));
   xmlWriter.addAttr("version", mVersion);
   xmlWriter.addAttr("build_revision", buildRevision);
   xmlWriter.addAttr("release_date", releaseText);
   xmlWriter.popAddPoint();

   if (pSettings == NULL)
   {
      return false;
   }

   xmlWriter.pushAddPoint(xmlWriter.addElement("group"));
   xmlWriter.addAttr("name", "settings");
   pSettings->toXml(&xmlWriter);
   xmlWriter.popAddPoint();

   if(saveMru && !mMruFiles.empty())
   {
      xmlWriter.pushAddPoint(xmlWriter.addElement("group"));
      xmlWriter.addAttr("name", "MRUFiles");
      for (vector<MruFile>::const_iterator iter = mMruFiles.begin(); iter != mMruFiles.end(); ++iter)
      {
         MruFile mruFile = *iter;
         if(mruFile.mName.empty() || mruFile.mImporterName.empty() || mruFile.mDescriptors.empty())
         {
            continue;
         }
         xmlWriter.pushAddPoint(xmlWriter.addElement("attribute"));
         xmlWriter.addAttr("name", mruFile.mName);
         xmlWriter.addAttr("importer", mruFile.mImporterName);

         vector<ImportDescriptor*>::iterator descriptorIter;
         for (descriptorIter = mruFile.mDescriptors.begin();
              descriptorIter != mruFile.mDescriptors.end();
              ++descriptorIter)
         {
            ImportDescriptor* pImportDescriptor = *descriptorIter;
            if (pImportDescriptor != NULL)
            {
               DataDescriptor* pDescriptor = pImportDescriptor->getDataDescriptor();
               if (pDescriptor != NULL)
               {
                  xmlWriter.pushAddPoint(xmlWriter.addElement("DataDescriptor"));
                  pDescriptor->toXml(&xmlWriter);
                  xmlWriter.popAddPoint();
               }
            }
         }

         const DateTime* pDateTime = &(mruFile.mModificationTime);
         string modificationText = StringUtilities::toXmlString(pDateTime);
         xmlWriter.addAttr("modification_time", modificationText);

         xmlWriter.popAddPoint();
      }

      xmlWriter.popAddPoint();
   }

   FILE* pFile = fopen(filename.c_str(), "wt");
   if (pFile != NULL)
   {
      xmlWriter.writeToFile(pFile);
      fclose(pFile);
      return true;
   }

   return false;
}
