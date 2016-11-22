/*
 *  Author: bwilliams
 *  Created: April 6, 2012
 *
 *  Copyright (C) 2012-2016 VMware, Inc.  All rights reserved. -- VMware Confidential
 *
 *  This code was generated by the script "build/dev/codeGen/genCppXml". Please
 *  speak to Brian W. before modifying it by hand.
 *
 */

#ifndef FullPackageElemXml_h_
#define FullPackageElemXml_h_


#include "Doc/CafInstallRequestDoc/CFullPackageElemDoc.h"

#include "Doc/DocXml/CafInstallRequestXml/CafInstallRequestXmlLink.h"
#include "Xml/XmlUtils/CXmlElement.h"

namespace Caf {

	/// Streams the FullPackageElem class to/from XML
	namespace FullPackageElemXml {

		/// Adds the FullPackageElemDoc into the XML.
		void CAFINSTALLREQUESTXML_LINKAGE add(
			const SmartPtrCFullPackageElemDoc fullPackageElemDoc,
			const SmartPtrCXmlElement thisXml);

		/// Parses the FullPackageElemDoc from the XML.
		SmartPtrCFullPackageElemDoc CAFINSTALLREQUESTXML_LINKAGE parse(
			const SmartPtrCXmlElement thisXml);
	}
}

#endif
