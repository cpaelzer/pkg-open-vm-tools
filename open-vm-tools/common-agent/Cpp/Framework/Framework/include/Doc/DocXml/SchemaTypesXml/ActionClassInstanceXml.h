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

#ifndef ActionClassInstanceXml_h_
#define ActionClassInstanceXml_h_


#include "Doc/SchemaTypesDoc/CActionClassInstanceDoc.h"

#include "Doc/DocXml/SchemaTypesXml/SchemaTypesXmlLink.h"
#include "Xml/XmlUtils/CXmlElement.h"

namespace Caf {

	/// Streams the ActionClassInstance class to/from XML
	namespace ActionClassInstanceXml {

		/// Adds the ActionClassInstanceDoc into the XML.
		void SCHEMATYPESXML_LINKAGE add(
			const SmartPtrCActionClassInstanceDoc actionClassInstanceDoc,
			const SmartPtrCXmlElement thisXml);

		/// Parses the ActionClassInstanceDoc from the XML.
		SmartPtrCActionClassInstanceDoc SCHEMATYPESXML_LINKAGE parse(
			const SmartPtrCXmlElement thisXml);
	}
}

#endif
