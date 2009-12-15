$(eval $(call test,object_test,jmvcc arch boost_thread-mt,boost))
$(eval $(call test,versioned_test,jmvcc arch boost_thread-mt,boost))
