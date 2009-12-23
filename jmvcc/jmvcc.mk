JMVCC_SOURCES := \
	snapshot.cc \
	sandbox.cc \
	transaction.cc \
	versioned_object.cc \
	garbage.cc

JMVCC_LINK :=  boost_date_time-mt

$(eval $(call library,jmvcc,$(JMVCC_SOURCES),$(JMVCC_LINK)))

$(eval $(call include_sub_make,jmvcc_testing,testing))
