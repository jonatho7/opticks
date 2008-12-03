/*
 * The information in this file is
 * Copyright(c) 2007 Ball Aerospace & Technologies Corporation
 * and is subject to the terms and conditions of the
 * GNU Lesser General Public License Version 2.1
 * The license text is available from   
 * http://www.gnu.org/licenses/lgpl.html
 */

#include "AppConfig.h"
#if defined(WIN_API)
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#include <pwd.h>
#endif

#include <string>
#include <sstream>

using namespace std;

#include "ApplicationServices.h"
#include "AppVersion.h"
#include "ConfigurationSettings.h"
#include "AppVerify.h"
#include "Descriptors.h"
#include "Filename.h"
#include "FileResource.h"
#include "MessageLogResource.h"
#include "ObjectFactory.h"
#include "ObjectResource.h"
#include "PlugInArg.h"
#include "PlugInArgList.h"
#include "PlugInManagerServices.h"
#include "ProductView.h"
#include "ScriptPlugIn.h"
#include "StringUtilities.h"
#include "UtilityServices.h"

ScriptPlugIn::ScriptPlugIn(int scriptIndex)
{
   // set description values
   if (static_cast<int>(gDescriptors.size()) <= scriptIndex)
   {
      setName("RunScript");
      setVersion(APP_VERSION_NUMBER);
      setCreator("Ball Aerospace & Technologies Corp.");
      setCopyright(APP_COPYRIGHT);
      setShortDescription("Runs a script file");
      setDescription("Runs the script file specified in the arg list");
      setMenuLocation("");
      mDescriptor.mScriptPath = "";
   }
   else
   {
      setName(gDescriptors[scriptIndex].mPlugInName);
      setVersion(gDescriptors[scriptIndex].mVersion.c_str());
      setCreator(gDescriptors[scriptIndex].mCreator);
      setCopyright(gDescriptors[scriptIndex].mCopyright);
      setShortDescription(gDescriptors[scriptIndex].mShortDescription);
      setDescription(gDescriptors[scriptIndex].mDescription);
      setMenuLocation(gDescriptors[scriptIndex].mMenuLocation);

      mDescriptor = gDescriptors[scriptIndex];
   }
   setDescriptorId("{DF1A9C13-C2EE-4795-8556-DE26CF302754}");
   allowMultipleInstances(true);
   setProductionStatus(APP_IS_PRODUCTION_RELEASE);
}

ScriptPlugIn::~ScriptPlugIn()
{
}

bool ScriptPlugIn::getInputSpecification(PlugInArgList *&pArgList)
{
   if (mDescriptor.mScriptPath == "")
   {
      Service<PlugInManagerServices> pPlugInManager;
      VERIFY(pPlugInManager.get() != NULL);
      pArgList = pPlugInManager->getPlugInArgList();
      VERIFY(pArgList != NULL);
      PlugInArg* p1 = pPlugInManager->getPlugInArg();
      VERIFY(p1 != NULL);
      p1->setName("ScriptPath");
      p1->setType("Filename");
      p1->setDefaultValue(NULL);
      pArgList->addArg(*p1);
      return true;
   }

   return false;
}

bool ScriptPlugIn::getOutputSpecification(PlugInArgList *&pArgList)
{
   Service<PlugInManagerServices> pPlugInManager;
   VERIFY(pPlugInManager.get() != NULL);
   pArgList = pPlugInManager->getPlugInArgList();
   VERIFY(pArgList != NULL);
   PlugInArg* p1 = pPlugInManager->getPlugInArg();
   VERIFY(p1 != NULL);
   p1->setName("ReturnPath");
   p1->setType("Filename");
   p1->setDefaultValue(NULL);
   pArgList->addArg(*p1);
   p1 = pPlugInManager->getPlugInArg();
   VERIFY(p1 != NULL);
   p1->setName("ReturnString");
   p1->setType("string");
   p1->setDefaultValue(NULL);
   pArgList->addArg(*p1);
   return true;
}

static void setEnvironmentVariable(const char *pName, const char *pValue)
{
   stringstream command;
   if ((pName == NULL) || (pValue == NULL))
   {
      return;
   }
   string buffer2(pValue);
#if defined(WIN_API)
   for (unsigned int i = 0; i < buffer2.size(); ++i)
   {
      if (buffer2[i] == '/')
      {
         buffer2[i] = '\\';
      }
   }
#endif
   command << pName << "=" << buffer2;
#if defined(WIN_API)
   _putenv(command.str().c_str());
#else
   putenv(const_cast<char*>(command.str().c_str()));
#endif
}

static void setEnvironmentOption(const Filename* pSettingValue, const string& environName)
{
   string value;
   if (pSettingValue != NULL)
   {
      value = pSettingValue->getFullPathAndName();
   }
   if (!value.empty())
   {
#if defined(WIN_API)
      vector<char> buffer(value.size() + 1);
      buffer[0] = '\0';
      GetShortPathName(value.c_str(), &buffer[0], buffer.size());
      setEnvironmentVariable(environName.c_str(), &buffer[0]);
#else
      setEnvironmentVariable(environName.c_str(), value.c_str());
#endif
   }
}

bool ScriptPlugIn::execute(PlugInArgList *pInArgList, PlugInArgList *pOutArgList)
{
   StepResource pStep("Execute script", "app", "2E423CEE-637F-4f03-BFE8-05E8235F55E0");

   VERIFY(pInArgList != NULL);
   VERIFY(pOutArgList != NULL);

   string scriptPath = mDescriptor.mScriptPath;
   PlugInArg* pArg = NULL;
   pInArgList->getArg("ScriptPath", pArg);
   if (pArg != NULL)
   {
      Filename* pFilename = pArg->getPlugInArgValue<Filename>();
      if (pFilename != NULL)
      {
         scriptPath = pFilename->getFullPathAndName().c_str();
         pStep->addProperty("Script File", scriptPath);
      }
   }
   setEnvironmentOption(ConfigurationSettings::getSettingExportPath(), "EXPORT_PATH");
   setEnvironmentOption(ConfigurationSettings::getSettingImportPath(), "IMPORT_PATH");
   setEnvironmentOption(ConfigurationSettings::getSettingMessageLogPath(), "MESSAGE_LOG_PATH");
   setEnvironmentOption(ConfigurationSettings::getSettingSupportFilesPath(), "SUPPORT_FILES_PATH");
   setEnvironmentOption(ProductView::getSettingTemplatePath(), "TEMPLATE_PATH");
   setEnvironmentOption(ConfigurationSettings::getSettingWizardPath(), "WIZARD_PATH");
   setEnvironmentOption(ConfigurationSettings::getSettingTempPath(), "OPTICKS_TEMP_PATH");

   Service<ConfigurationSettings> pConfig;
   ConfigurationSettingsExt2* pSettings = dynamic_cast<ConfigurationSettingsExt2*>(pConfig.get());
   VERIFY(pSettings != NULL);
   string homePath = pConfig->getHome();
   setEnvironmentVariable("OPTICKS_HOME", homePath.c_str());
   setEnvironmentVariable("PLUG_IN_PATH", pSettings->getPlugInPath().c_str());

   bool success = true;
#if defined(WIN_API)
   vector<char> shortPathName(scriptPath.size()+1);
   shortPathName[0] = '\0';
   GetShortPathName(scriptPath.c_str(), &shortPathName[0], shortPathName.size());
   success = (0 == spawnl(_P_WAIT, &shortPathName[0], &shortPathName[0], NULL));
#else
   scriptPath = "'" + scriptPath + "'";
   system(scriptPath.c_str());
#endif

   if (!success)
   {
      pStep->finalize(Message::Failure, "Script Failed");
   }
   else
   {
      populateOutputArgList(pOutArgList);
      pStep->finalize(Message::Success);
   }

   return success;
}

static bool getUserName(string &userName)
{
#if defined(WIN_API)
   char pUserName[512];
   DWORD size = 512;
   VERIFY(GetUserName(pUserName, &size) != false);
   userName = pUserName;
#else
   struct passwd* userEntry = getpwuid(getuid());
   VERIFY(userEntry != NULL);
   VERIFY(userEntry->pw_name != NULL);
   userName = userEntry->pw_name;
#endif
   return true;
}

bool ScriptPlugIn::populateOutputArgList(PlugInArgList *pOutArgList)
{
   VERIFY(pOutArgList != NULL);

   bool success = true;

   string tempPath;
   const Filename* pTempPath = ConfigurationSettings::getSettingTempPath();
   if (pTempPath != NULL)
   {
      tempPath = pTempPath->getFullPathAndName();
   }

   string userName;
   VERIFY(getUserName(userName) != false);

   string filename = tempPath + SLASH + "script_output_" + userName + ".txt";

   FileResource pFile(filename.c_str(), "r");
   if (pFile.get() == NULL)
   {
      // Not necessarily a bug, so don't use VERIFY
      return false;
   }

   const int BUFFER_SIZE = 1024;
   char buffer[BUFFER_SIZE];
   if (fgets(buffer, BUFFER_SIZE, pFile) == NULL)
   {
      return false;
   }
   string output(buffer);
   output = StringUtilities::stripWhitespace(output);

   FactoryResource<Filename> pFilename;
   VERIFY(pFilename.get() != NULL);
   pFilename->setFullPathAndName(output.c_str());

   FileResource pOutput(pFilename->getFullPathAndName().c_str(), "r");
   if (pOutput.get() != NULL)
   {
      output = pFilename->getFullPathAndName();
      PlugInArg* pArg = NULL;
      VERIFY(pOutArgList->getArg("ReturnPath", pArg) && (pArg != NULL));
      pArg->setActualValue(pFilename.release());
   }
   else
   {
      output = buffer;
   }

   PlugInArg* pArg = NULL;
   VERIFY(pOutArgList->getArg("ReturnString", pArg) && (pArg != NULL));
   pArg->setActualValue(&output);

   return success;
}

bool ScriptPlugIn::initialize()
{
   return false;
}
